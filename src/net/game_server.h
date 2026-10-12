// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once
/// ============================================================================
/// UDP Game Server — UE5 StatelessConnect Handshake + Relay + Replay + LiveWorld
/// ============================================================================
///
/// TABLE OF CONTENTS (~line ranges; searchable by label)
/// -----------------------------------------------------
///   OS/socket compat shims ................. top of file
///   Wire primitives include ................ ~85
///   [ue5_hs] namespace (HandshakePacket, Intrepid ext) ~100-770
///   [ReplayData] struct (load + patch methods) ~370-770
///   [ClientState] struct ..................... ~770
///   write_sc_packet_prefix (shared S>C helper) ~890
///   [GameServer]::Config ..................... ~920
///   [GameServer] ctor ........................ ~960
///   [GameServer]::start / stop ............... ~1060
///   generate_cookie (HMAC-SHA1) .............. ~1270
///   send_to_client ........................... ~1315
///   resolve_relay_target (DNS) ............... ~1330
///   handle_packet (top-level dispatcher) ..... ~1360
///   handle_handshake_initial ................. ~1380
///   handle_handshake_response (+ HANDSHAKE COMPLETE) ~1470
///   handle_game_data (recv loop for NMT/data) . ~1630
///   send_nmt / send_nmt_challenge / welcome .. ~2170
///   bootstrap_loop (embedded-bootstrap path) . ~2700
///   build_replay_packet ...................... ~3370
///   replay_loop .............................. ~3510
///   run_loop / keepalive_tick / upstream_loop  ~4170
///
/// OPERATING MODES
/// ---------------
///   1. EMULATION (default):
///      UE5 StatelessConnect handshake (Initial → Challenge → Response → Ack)
///      plus Intrepid auth ext (CharID + Token).  After handshake either
///      replay or bootstrap kicks in.
///
///   2. RELAY (config.relay_target):
///      UDP forwarded to an upstream server.
///
///   3. REPLAY / EMBEDDED BOOTSTRAP (--replay / --use-embedded-bootstrap):
///      After handshake, stream captured bytes so the client enters world.
///      The only currently-working end-to-end path.
///
///   4. LIVEWORLD observer (--enable-live-world, Session F):
///      Constructs the Session/World/Emit pipeline alongside the replay path.
///      Observes real clients in SessionRegistry; heartbeat proves the tick
///      thread runs at replication_hz.  Does not drive packets.
///
///   5. LIVEWORLD active (Session G — THIS session):
///      Same pipeline, but on handshake-complete the player's PC/Pawn/PS
///      are spawned in LiveWorld's ActorRegistry via CharacterProfile; the
///      BroadcastManager drives UdpPacketEmitter which produces complete
///      UE5 packets (outer header + bunch + termination).  Byte production
///      is always on; byte SENDING is gated by --session-g-send (default
///      off for safety while the path is new).
///
/// WIRE PROTOCOL (worked out from captures, kept stable across sessions)
/// ---------------------------------------------------------------
///   - MagicHeader: 32 bits = 0x500c7696 (bytes: 96 76 0c 50)
///   - SessionID: 2 bits, ClientID: 3 bits
///   - HandshakeBit: 1 bit (1=handshake, 0=game data)
///   - EHandshakeVersion: MinVersion=3 (SessionClientId), CurVersion=4 (Latest)
///   - NetworkVersion: 0xa42f624e (confirmed from MITM capture 2026-02-18)
///   - Handshake types: 0=Initial, 1=Challenge, 2=Response, 3=Ack
///   - Custom Intrepid extension: Marker(6B) + CharID(FGuid) + Token(FGuid)
///   - Termination bit at end of every packet (PacketHandler convention)
// -- C | O | A x C-Y-B --

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
inline int close_socket(socket_t s) { return closesocket(s); }
inline void init_sockets() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/hmac.h>
using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
inline int close_socket(socket_t s) { return close(s); }
inline void init_sockets() {}
#endif

#include <thread>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <random>
#include <array>
#include <optional>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <memory>
#include "util/proxy_logger.h"
#include "data/bootstrap_data.h"
#include "net/bunch_generator.h"
#include "generators/generators.h"
#include "protocol/bootstrap/bootstrap_sequence.h"
#include "protocol/character_profile.h"
#include "protocol/net_guid_allocator.h"
// Note: protocol/emit/replay_mutator.h is intentionally NOT included here.
// The ReplayMutator library is kept in tree (and exercised by
// test_replay_mutator.cpp) but not wired into the live replay path —
// see the big comment in replay_loop() for the why.

// ─── Session F: Live-world integration (additive) ───────────────────────────
// The LiveWorld shim bundles SessionRegistry + OpcodeDispatcher +
// ActorRegistry + VisibilityManager + BroadcastManager + IPacketEmitter into
// a single opt-in object.  GameServer wires it in when
// Config::enable_live_world is set; by default the legacy handshake/replay
// flow runs untouched.
#include "net/live_world.h"
#include "net/nmt_opcodes.h"
#include "protocol/schema/schema_registry.h"

// ─── Phase III M1: native property-update bunches ───────────────────────────
// PropertyUpdateBunchBuilder produces non-partial property-delta bunches
// (e.g. Name update).  Used by NativeConnectSequencer in --native mode
// (not wired into the replay path — that experiment was abandoned).
#include "protocol/emit/property_update_bunch_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_info.h"
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/level_visibility.h"
#include "protocol/wire/hit_confirmation.h"
#include "protocol/wire/possession.h"

// ─── Path B: authoritative-server connect orchestrator ─────────────────────
#include "net/native_connect_sequencer.h"

// ─── UE5 Bit Reader/Writer ─────────────────────────────────────────────────
//
// UE5 FBitReader uses LSB-first per byte. These helpers mirror that layout.

// ─── Wire primitives ────────────────────────────────────────────────────────
// All ue5:: bit-level + SerializeIntPacked helpers now live in the canonical
// protocol/wire/ue5_primitives.h — one source of truth for the 4-layer
// architecture.  This include makes them available under the same names.
#include "protocol/wire/ue5_primitives.h"
#include "protocol/wire/server_move_parser.h"
#include "protocol/wire/actor_rpc_parser.h"
#include "protocol/tools/replay_property_patcher.h"

// ─── Shared S>C bunch parser ────────────────────────────────────────────────
// Include AFTER ue5:: is defined (the parser uses ue5::read_bits and
// ue5::read_serialize_int).  Included at file scope so any downstream
// code in this header (and any source including this header) has access
// to aoc::parse_sc_bunch / aoc::BunchSummary / aoc::BunchKind.
#include "net/sc_bunch_parser.h"

// ─── UE5 Handshake Constants ────────────────────────────────────────────────

namespace ue5_hs {

// Handshake version enum (matches UE5 EHandshakeVersion)
enum HandshakeVersion : uint8_t {
    Original            = 0,
    Randomized          = 1,
    NetCLVersion        = 2,
    SessionClientId     = 3,
    NetCLUpgradeMessage = 4,
    Latest              = NetCLUpgradeMessage
};

// Handshake packet types
enum PacketType : uint8_t {
    Initial         = 0,
    Challenge       = 1,
    Response        = 2,
    Ack             = 3,
    Restart         = 4,
    RestartResponse = 5,
    VersionUpgrade  = 6,
};

// Sizes
constexpr int COOKIE_SIZE       = 20;   // SHA-1 output
constexpr int SECRET_SIZE       = 64;   // HMAC key
constexpr int MAGIC_HEADER_BITS = 32;   // AOC's MagicHeader

// Intrepid custom extension marker
constexpr uint8_t INTREPID_MARKER[6] = { 0x7f, 0x7f, 0x7f, 0x7f, 0xff, 0x7f };
// (C.Y.B | CoA)

/// Parsed handshake fields from a client packet.
struct HandshakePacket {
    uint32_t magic_header    = 0;
    uint8_t  session_id      = 0;
    uint8_t  client_id       = 0;
    bool     is_handshake    = false;
    bool     restart         = false;
    uint8_t  min_version     = 0;
    uint8_t  cur_version     = 0;
    uint8_t  packet_type     = 0;
    uint8_t  sent_count      = 0;
    uint32_t network_version = 0;
    uint16_t network_features= 0;
    uint8_t  secret_id       = 0;
    double   timestamp       = 0.0;
    uint8_t  cookie[COOKIE_SIZE] = {};

    // Custom Intrepid extension
    bool     has_custom_ext  = false;
    std::string char_id;     // hex GUID (32 chars)
    std::string token;       // hex GUID (32 chars)
    uint8_t  char_id_raw[16] = {};
    uint8_t  token_raw[16]   = {};
};

/// Parse a handshake packet.
inline bool parse(const uint8_t* data, size_t len, HandshakePacket& pkt) {
    if (len < 10) return false;
    size_t eff_bits = ue5::strip_termination(data, len);
    size_t off = 0;

    pkt.magic_header    = static_cast<uint32_t>(ue5::read_bits(data, len, off, 32));
    pkt.session_id      = static_cast<uint8_t>(ue5::read_bits(data, len, off, 2));
    pkt.client_id       = static_cast<uint8_t>(ue5::read_bits(data, len, off, 3));
    pkt.is_handshake    = ue5::read_bits(data, len, off, 1) != 0;
    if (!pkt.is_handshake) return true; // game data, not handshake

    pkt.restart         = ue5::read_bits(data, len, off, 1) != 0;
    pkt.min_version     = static_cast<uint8_t>(ue5::read_bits(data, len, off, 8));
    pkt.cur_version     = static_cast<uint8_t>(ue5::read_bits(data, len, off, 8));
    pkt.packet_type     = static_cast<uint8_t>(ue5::read_bits(data, len, off, 8));
    pkt.sent_count      = static_cast<uint8_t>(ue5::read_bits(data, len, off, 8));
    pkt.network_version = static_cast<uint32_t>(ue5::read_bits(data, len, off, 32));
    pkt.network_features= static_cast<uint16_t>(ue5::read_bits(data, len, off, 16));

    // Handshake-specific data
    pkt.secret_id       = static_cast<uint8_t>(ue5::read_bits(data, len, off, 1));

    // Timestamp (64 bits = double)
    uint64_t ts_raw     = ue5::read_bits(data, len, off, 64);
    std::memcpy(&pkt.timestamp, &ts_raw, 8);

    // Cookie (20 bytes)
    for (int i = 0; i < COOKIE_SIZE; ++i)
        pkt.cookie[i] = static_cast<uint8_t>(ue5::read_bits(data, len, off, 8));

    // Standard UE5 handshake data ends here (bit 344 typically)
    // Check for custom Intrepid extension
    size_t remaining_bits = eff_bits > off ? eff_bits - off : 0;
    if (remaining_bits >= (6 + 16 + 16) * 8) { // marker + 2 GUIDs minimum
        // Read remaining bytes and look for marker + GUIDs
        std::vector<uint8_t> custom;
        while (off + 8 <= eff_bits) {
            custom.push_back(static_cast<uint8_t>(ue5::read_bits(data, len, off, 8)));
        }

        // Find marker pattern
        for (size_t i = 0; i + 5 < custom.size(); ++i) {
            if (std::memcmp(custom.data() + i, INTREPID_MARKER, 6) == 0) {
                pkt.has_custom_ext = true;
                // After marker: 16B zeros + 16B char_id + 16B zeros + 16B token
                size_t p = i + 6;
                p += 16; // skip zeros
                if (p + 16 <= custom.size()) {
                    std::memcpy(pkt.char_id_raw, custom.data() + p, 16);
                    pkt.char_id = ue5::guid_to_hex(pkt.char_id_raw);
                    p += 16;
                }
                p += 16; // skip zeros
                if (p + 16 <= custom.size()) {
                    std::memcpy(pkt.token_raw, custom.data() + p, 16);
                    pkt.token = ue5::guid_to_hex(pkt.token_raw);
                }
                break;
            }
        }
    }

    return true;
}

/// ── Intrepid custom extension data for Challenge/Ack packets ─────────
///
/// MITM relay capture (2026-02-18, an official server on port 7229) reveals
/// the REAL AoC server Challenge is 153 bytes (not 72!).  After the
/// standard UE5 header (43 bytes = magic + session + client + hs + versions
/// + secretId + timestamp + cookie), the server echoes the Intrepid
/// extension with:
///
///   +0..+21  (22B) : random server data (replaces marker 6B + zeros 16B)
///   +22..+37 (16B) : CharID echoed from client's Initial
///   +38..+53 (16B) : random server data (replaces zeros)
///   +54..+69 (16B) : Token echoed from client's Initial
///   +70..+93 (24B) : spawn coordinates (3 × double LE: X, Y, Z)
///                    = 94 bytes of structured extension
///   then ~15B random trailing padding + termination bit
///
/// The Ack (147B) uses the SAME 94-byte extension block, but with
/// ~9 bytes of trailing padding.
///
/// The Response from the client echoes bytes 43-136 of the Challenge
/// (the full 94-byte extension) and adds its own ~11B trailing padding.
///
constexpr int INTREPID_EXT_SIZE = 94; // 22 + 16 + 16 + 16 + 24

struct IntrepidExtData {
    uint8_t data[INTREPID_EXT_SIZE] = {};
    bool    present = false;

    /// Generate extension block from parsed Initial data.
    void generate(const uint8_t* char_id_raw, const uint8_t* token_raw,
                  double spawn_x, double spawn_y, double spawn_z) {
        present = true;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 255);

        size_t pos = 0;
        // 22 bytes random server data (replaces marker 6B + zeros 16B)
        for (int i = 0; i < 22; ++i)
            data[pos++] = static_cast<uint8_t>(dist(gen));
        // 16 bytes CharID (echoed from Initial)
        std::memcpy(data + pos, char_id_raw, 16); pos += 16;
        // 16 bytes random server data (replaces zeros between CharID/Token)
        for (int i = 0; i < 16; ++i)
            data[pos++] = static_cast<uint8_t>(dist(gen));
        // 16 bytes Token (echoed from Initial)
        std::memcpy(data + pos, token_raw, 16); pos += 16;
        // 24 bytes: spawn coordinates (3 doubles, little-endian)
        std::memcpy(data + pos, &spawn_x, 8); pos += 8;
        std::memcpy(data + pos, &spawn_y, 8); pos += 8;
        std::memcpy(data + pos, &spawn_z, 8); pos += 8;
        // total = 22 + 16 + 16 + 16 + 24 = 94
    }
};
// CoA and C:Y:B

/// Build a handshake packet (Challenge or Ack).
/// Returns byte length of the constructed packet.
///
/// When `ext` is provided and present, the Intrepid extension (94 bytes)
/// is written after the cookie, followed by `trail_pad` random bytes.
/// This produces packets matching real AoC server sizes:
///   Challenge: 43 + 94 + 15 + term = 153 bytes
///   Ack:       43 + 94 +  9 + term = 147 bytes
///
/// When `ext` is null/not-present, falls back to 28 random bytes (72B).
inline size_t build(uint8_t* buf, size_t buf_cap,
                    const HandshakePacket& client_pkt,
                    PacketType type,
                    uint8_t sent_count,
                    uint8_t secret_id,
                    double timestamp,
                    const uint8_t* cookie,
                    const IntrepidExtData* ext = nullptr,
                    int trail_pad = 28) {
    std::memset(buf, 0, buf_cap);
    size_t off = 0;

    // ── Header (echo client's values) ───────────────────────────────
    ue5::write_bits(buf, buf_cap, off, client_pkt.magic_header, 32);
    ue5::write_bits(buf, buf_cap, off, client_pkt.session_id, 2);
    ue5::write_bits(buf, buf_cap, off, client_pkt.client_id, 3);
    ue5::write_bits(buf, buf_cap, off, 1, 1);  // HandshakeBit = 1
    ue5::write_bits(buf, buf_cap, off, 0, 1);  // RestartBit = 0

    ue5::write_bits(buf, buf_cap, off, client_pkt.min_version, 8);
    ue5::write_bits(buf, buf_cap, off, client_pkt.cur_version, 8);
    ue5::write_bits(buf, buf_cap, off, type, 8);
    ue5::write_bits(buf, buf_cap, off, sent_count, 8);
    ue5::write_bits(buf, buf_cap, off, client_pkt.network_version, 32);
    ue5::write_bits(buf, buf_cap, off, client_pkt.network_features, 16);

    // ── SecretId ────────────────────────────────────────────────────
    ue5::write_bits(buf, buf_cap, off, secret_id, 1);

    // ── Timestamp (64 bits = double) ────────────────────────────────
    uint64_t ts_raw;
    std::memcpy(&ts_raw, &timestamp, 8);
    ue5::write_bits(buf, buf_cap, off, ts_raw, 64);

    // ── Cookie (20 bytes) ───────────────────────────────────────────
    for (int i = 0; i < COOKIE_SIZE; ++i)
        ue5::write_bits(buf, buf_cap, off, cookie[i], 8);

    // ── Intrepid extension (94 bytes, when present) ─────────────────
    if (ext && ext->present) {
        for (int i = 0; i < INTREPID_EXT_SIZE; ++i)
            ue5::write_bits(buf, buf_cap, off, ext->data[i], 8);
    }

    // ── Trailing random padding ─────────────────────────────────────
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (int i = 0; i < trail_pad; ++i)
        ue5::write_bits(buf, buf_cap, off, dist(gen), 8);

    // ── Termination bit ─────────────────────────────────────────────
    return ue5::add_termination(buf, buf_cap, off);
}

} // namespace ue5_hs

// ─── Packet Replay System ───────────────────────────────────────────────────
//
// Captures S>C packets from a real server relay session and replays them
// in a local-only emulation mode.  The replay file stores raw packet bytes
// with metadata; the engine rewrites outer headers (magic, session, client,
// seq/ack, custom field) to match the current session.

struct ReplayPacketInfo {
    uint32_t timestamp_ms;      // relative to first packet
    uint16_t original_seq;
    uint16_t original_ack;
    uint16_t bunch_start_bit;   // bit offset where bunch data starts
    uint16_t bunch_bits;        // number of bunch data bits
    uint8_t  has_pkt_info;
    uint8_t  has_srv_frame;
    uint8_t  frame_time;
    uint16_t jitter;
    uint8_t  hist_count;
    std::vector<uint8_t> raw;   // full original packet bytes
};
// built with C Y B and CoA

struct ReplayData {
    static constexpr uint32_t MAGIC = 0x52504C59; // 'RPLY'
    uint32_t packet_count = 0;
    uint8_t  server_custom_field[6] = {};
    uint8_t  client_custom_field[6] = {};
    uint8_t  session_id = 0;
    uint8_t  client_id = 0;
    uint16_t initial_seq = 0;
    uint16_t initial_ack = 0;
    std::vector<ReplayPacketInfo> packets;

    // (Removed 2026-04-23: allow_variable_name toggle.  The patcher it
    //  gated is gone.)

    bool load(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            spdlog::error("[Replay] Cannot open: {}", path);
            return false;
        }

        uint32_t magic, version, count;
        file.read(reinterpret_cast<char*>(&magic), 4);
        file.read(reinterpret_cast<char*>(&version), 4);
        file.read(reinterpret_cast<char*>(&count), 4);

        if (magic != MAGIC || version != 1) {
            spdlog::error("[Replay] Invalid file: magic=0x{:08X} ver={}", magic, version);
            return false;
        }

        file.read(reinterpret_cast<char*>(server_custom_field), 6);
        file.read(reinterpret_cast<char*>(client_custom_field), 6);
        file.read(reinterpret_cast<char*>(&session_id), 1);
        file.read(reinterpret_cast<char*>(&client_id), 1);
        file.read(reinterpret_cast<char*>(&initial_seq), 2);
        file.read(reinterpret_cast<char*>(&initial_ack), 2);

        // Reserved
        char reserved[4];
        file.read(reserved, 4);

        packet_count = count;
        packets.resize(count);

        uint32_t loaded = 0;
        for (uint32_t i = 0; i < count; ++i) {
            auto& p = packets[i];
            uint16_t raw_size;
            file.read(reinterpret_cast<char*>(&p.timestamp_ms), 4);
            file.read(reinterpret_cast<char*>(&raw_size), 2);
            file.read(reinterpret_cast<char*>(&p.original_seq), 2);
            file.read(reinterpret_cast<char*>(&p.original_ack), 2);
            file.read(reinterpret_cast<char*>(&p.bunch_start_bit), 2);
            file.read(reinterpret_cast<char*>(&p.bunch_bits), 2);
            file.read(reinterpret_cast<char*>(&p.has_pkt_info), 1);
            file.read(reinterpret_cast<char*>(&p.has_srv_frame), 1);
            file.read(reinterpret_cast<char*>(&p.frame_time), 1);
            file.read(reinterpret_cast<char*>(&p.jitter), 2);
            file.read(reinterpret_cast<char*>(&p.hist_count), 1);
            if (file.fail()) {
                spdlog::error("[Replay] EOF reading packet #{} metadata", i);
                break;
            }
            if (raw_size == 0 || raw_size > 65000) {
                spdlog::warn("[Replay] Pkt #{}: suspicious raw_size={}, skipping", i, raw_size);
                packets.resize(i);
                break;
            }
            p.raw.resize(raw_size);
            file.read(reinterpret_cast<char*>(p.raw.data()), raw_size);
            if (file.fail()) {
                spdlog::error("[Replay] EOF reading packet #{} raw data ({}B)", i, raw_size);
                packets.resize(i);
                break;
            }
            // Log first few packets for sanity check
            if (i < 3) {
                spdlog::info("[Replay]  Pkt[{}]: raw={}B seq={} bsb={} bb={} hasSrv={}",
                             i, raw_size, p.original_seq, p.bunch_start_bit,
                             p.bunch_bits, (int)p.has_srv_frame);
            }
            ++loaded;
        }

        spdlog::info("[Replay] Loaded {}/{} packets from {} "
                     "(initSeq={} custom={:02x}{:02x}{:02x}{:02x}{:02x}{:02x})",
                     loaded, count, path, initial_seq,
                     server_custom_field[0], server_custom_field[1],
                     server_custom_field[2], server_custom_field[3],
                     server_custom_field[4], server_custom_field[5]);
        return true;
    }

    // ─── FString patcher REMOVED (2026-04-23) ──────────────────────────
    //
    // Everything that used to live in this region — NameSite,
    // kPawnNametagSite / kHudNameSite, kOriginalNameChars / kMaxNameChars,
    // legacy kName* / kPawnName* aliases, ReplayData's private bit
    // read/write primitives, walk_2bit_partial_find_bdb,
    // find_name_bunch_bdb_bit, apply_safe_10char_patch,
    // patch_fstring_variable, patch_character_name, patch_pawn_nametag —
    // was machinery for post-hoc mutation of captured bunch bytes.
    //
    // That approach is dead.  Captured-packet patching was structurally
    // fragile: modifying a property's bytes requires cross-packet
    // invariants (BDB, subsequent bunch references, mask coherence) that
    // we don't fully own.  Even when every bit-level step was provably
    // correct, the client still rejected the packet.
    //
    // Replaced by full RepLayout synthesis.  Server-side state
    // (name, class, location, etc.) flows directly into
    // src/protocol/emit/ at packet-build time.  See the Phase II plan.
    //
    // Git history (commit preceding 2026-04-23 cleanup) has the old code
    // if any of it ever needs to be resurrected.

    // ── Phase M5.6b — Archetype (class) ID patch ─────────────────────────
    //
    // Located by scanning the 400-packet bootstrap for the 8 known
    // archetype IDs (17747..17754 = Bard..Tank) as u16-LE at every bit
    // offset.  Only one hit: pkt idx 22 (PlayerController ActorOpen),
    // bits 5823 and 5967, both encoding 17748 = Cleric.  RandomChar is
    // a Cleric.
    //
    // Two copies because archetype is replicated on both PlayerState
    // and PlayerController (or appears in two RepLayout Cmds on the
    // same actor).  Patching both keeps the captured stream internally
    // consistent.
    //
    // u16 means AoC uses a uint16_t enum/ID for archetype — 8 values
    // comfortably fit.  We write the low 16 bits of profile.archetype_id;
    // higher bits are silently dropped (they're zero for valid IDs).
    static constexpr size_t kArchetypePacketIndex = 22;
    static constexpr size_t kArchetypeBits        = 16;
    static constexpr size_t kArchetypeBit1        = 5823;
    static constexpr size_t kArchetypeBit2        = 5967;

    bool patch_archetype_id(uint64_t archetype_id) {
        if (packets.size() <= kArchetypePacketIndex) {
            spdlog::warn("[Replay] patch_archetype_id: packet index {} out "
                         "of range (have {})", kArchetypePacketIndex,
                         packets.size());
            return false;
        }
        if (archetype_id == 0) {
            spdlog::info("[Replay] patch_archetype_id: id=0, no patch");
            return false;
        }
        auto& pkt = packets[kArchetypePacketIndex];
        const size_t need = std::max(kArchetypeBit1, kArchetypeBit2) + kArchetypeBits;
        if (pkt.raw.size() * 8 < need) {
            spdlog::warn("[Replay] patch_archetype_id: packet too small "
                         "({}B, need {} bits)", pkt.raw.size(), need);
            return false;
        }
        const uint16_t value = static_cast<uint16_t>(archetype_id & 0xFFFF);
// [C | Y | B + C | O | A]

        auto splice_u16 = [&](size_t start_bit) {
            for (size_t i = 0; i < kArchetypeBits; ++i) {
                const int    src_bit = (value >> i) & 1;
                const size_t dbit    = start_bit + i;
                const uint8_t mask   = static_cast<uint8_t>(1u << (dbit & 7));
                if (src_bit) pkt.raw[dbit >> 3] |=  mask;
                else         pkt.raw[dbit >> 3] &= static_cast<uint8_t>(~mask);
            }
        };

        // Read the original value for logging (either of the two; both
        // should match).
        uint16_t original = 0;
        for (size_t i = 0; i < kArchetypeBits; ++i) {
            const size_t bp = kArchetypeBit1 + i;
            original |= static_cast<uint16_t>(((pkt.raw[bp >> 3] >> (bp & 7)) & 1) << i);
        }
        splice_u16(kArchetypeBit1);
        splice_u16(kArchetypeBit2);

        spdlog::info("[Replay] patch_archetype_id: {} -> {} "
                     "in pkt[{}] @ bits {} and {}",
                     original, value, kArchetypePacketIndex,
                     kArchetypeBit1, kArchetypeBit2);
        return true;
    }
};

// ─── Client Connection State ────────────────────────────────────────────────

struct ClientState {
    enum Phase {
        AWAITING_INITIAL,
        CHALLENGE_SENT,
        HANDSHAKE_COMPLETE,
        CONNECTED,
    };

    Phase        phase = AWAITING_INITIAL;
    sockaddr_in  addr{};
    uint8_t      secret[ue5_hs::SECRET_SIZE] = {};
    double       challenge_timestamp = 0.0;
    uint8_t      challenge_cookie[ue5_hs::COOKIE_SIZE] = {};
    uint8_t      secret_id = 1;             // Real AoC server uses 1
    uint8_t      server_sent_count = 0;
    std::string  char_id;
    std::string  token;
    std::chrono::steady_clock::time_point last_activity;
    uint32_t     magic_header = 0;
    uint32_t     network_version = 0;
    uint16_t     network_features = 0;
    uint8_t      min_version = 0;
    uint8_t      cur_version = 0;
    uint8_t      session_id = 0;
    uint8_t      client_id_bits = 0;
    // Sequence number tracking (14-bit, 0-16383)
    static constexpr uint16_t SEQ_MASK = 0x3FFF;
    uint16_t out_seq     = 0;   // Next outgoing sequence
    uint16_t in_ack_seq  = 0;   // Last incoming seq we acknowledge
    uint16_t out_ack_seq = 0;   // Remote's last ack of our seq
    uint16_t reliable_seq = 1;  // Outgoing reliable channel sequence (UE5 InReliable init=0, so first must be 1)
    uint16_t in_reliable_base = 0; // InReliable[ch] base = ServerSeq % 4096 (saved for bootstrap ChSeq patching)
    int      nmt_state   = 0;   // 0=awaiting_hello, 1=sent_challenge, etc.
    uint32_t pkt_recv_count = 0; // Data packets received from this client
    std::chrono::steady_clock::time_point last_keepalive_sent; // For periodic keepalives

    // Per-client bootstrap guard — true while bootstrap_loop is running for
    // this client.  Protected by client_mu_ in the recv loop; cleared by
    // bootstrap_loop itself (under client_mu_) when it finishes.
    bool bootstrap_active = false;

    // Intrepid extension data — generated during Challenge, reused for Ack
    ue5_hs::IntrepidExtData ext_data;

    // ── AoC custom packet field ────────────────────────────────────────
    // AoC inserts a custom field right after the UE5 FNetPacketNotify
    // header (packed header + history words), BEFORE standard PacketInfo.
    // ASYMMETRIC sizes:
    //   C>S: 48 bits (6 bytes) — 4 constant + 2 varying
    //   S>C: 56 bits (7 bytes) — 5 constant + 2 varying
    // The client rejects S>C packets with wrong field size
    // ("FaultDisconnect,NotRecoverable,ZeroLastByte").
    //
    // We capture the 6-byte C>S field and extend to 7 bytes for S>C
    // by appending a zero byte.
    static constexpr int CUSTOM_FIELD_BYTES = 6;   // SAME for both C>S and S>C
    static constexpr int CUSTOM_FIELD_BITS  = 48;  // 6 × 8 = 48 bits
    uint8_t  custom_field[6] = {};  // constant per session, from IntrepidExtData[0:6]
    bool     custom_field_captured = false;

    // ── Per-channel outgoing reliable chSeq tracker (Phase B.0e2, 2026-04-27)
    //
    // We need this because cs.reliable_seq above is a SINGLE counter advanced
    // by ch=0 control bunches.  Spliced bunches on other channels (e.g., ch=3
    // for the PC) carry their own captured chSeq values inside the bunch
    // payload.  When we want to send a NATIVE bunch on ch=3 (e.g., the
    // ClientAckUpdateLevelVisibility ack), it must use a chSeq that's
    // contiguous with what the splice has already shipped — otherwise the
    // client classifies it as out-of-window (>1024 bunches from
    // InReliable[ch]) and closes with NMT_Close.
    //
    // Updated by send_captured_packet (post-build parse) and any native send
    // path that emits a reliable bunch on a non-control channel.
    // Map key = ChIndex; value = LAST chSeq we sent on that channel.
    std::unordered_map<uint32_t, uint16_t> last_outgoing_reliable_chseq;

    // ── SULV ACK queue (Phase B.0e2, 2026-04-27) ──────────────────────────
    //
    // SULV (ServerUpdateLevelVisibility) RPCs arrive from the client during
    // the bootstrap splice phase.  Sending the ACK immediately would race
    // the splice (which still has more ch=3 reliable bunches to ship), and
    // we'd have to "reserve" a chSeq value that conflicts with future
    // captured chSeq values baked into the spliced bunches.  Instead we
    // queue ACKs here; drain after WorldBootstrap === complete fires.
    struct PendingSulvAck {
        aoc::protocol::wire::LevelVisibilityRequest request;  // decoded SULV
        uint32_t     triggering_ch_idx;  // logging only
    };
    std::vector<PendingSulvAck> pending_sulv_acks;
    bool world_bootstrap_complete = false;
    uint64_t hit_confirmations_sent = 0;   // InstGuid source for ClientHitConfirmations
    int  client_restarts_sent   = 0;       // ClientRestart answers to possession checks
    bool client_restart_pending = false;   // a check arrived while the replay was running
// ~ C:Y:B :: C-O-A ~

    // ── Phase B.0p4 (2026-04-28 PM) — reactive native ClientRestart ──
    // Set true by send_bunch_packet when it ships pkt#78 (PawnEmitter path)
    // — this is the moment NetGUID 88 (the captured Pawn) becomes resolvable
    // on the client.  Until this flag is set, any reactive ClientRestart we
    // emit would arrive before the client knows about the Pawn → it would
    // call AcknowledgePossession(NULL).
    bool pkt78_emitted = false;
    // Set true the first time we successfully ship a native ClientRestart
    // RPC for this client.  Idempotent guard so we don't keep re-sending
    // every time the client retries SNLW.
    bool reactive_clientrestart_sent = false;

    void init_sequences(int16_t server_seq, int16_t client_seq) {
        out_seq     = server_seq & SEQ_MASK;
        // UE5 InitSequence: InAckSeq = IncomingSequence - 1
        // ("last seq received" before any data arrives)
        in_ack_seq  = (client_seq - 1) & SEQ_MASK;
        out_ack_seq = (out_seq - 1) & SEQ_MASK;
        // Fix #14: AoC initializes InReliable[ch] = ServerSeq % 4096 (not 0).
        // Client expects first reliable ChSequence = InReliable[ch] + 1.
        // S>C ChSequence is 12-bit (MAX_CHSEQUENCE = 4096).
        in_reliable_base = out_seq & 0xFFF;
        reliable_seq = in_reliable_base + 1;
        nmt_state    = 0;
        pkt_recv_count = 0;
        last_keepalive_sent = std::chrono::steady_clock::now();
        spdlog::info("[GameServer]   Sequences init: OutSeq={} InAckSeq={} OutAckSeq={} ReliableSeq={} (InReliable[0]={})",
                     out_seq, in_ack_seq, out_ack_seq, reliable_seq, (out_seq & 0xFFF));
    }
    /// Copy header fields from parsed packet
    void init_from(const ue5_hs::HandshakePacket& pkt) {
        magic_header     = pkt.magic_header;
        network_version  = pkt.network_version;
        network_features = pkt.network_features;
        min_version      = pkt.min_version;
        cur_version      = pkt.cur_version;
        session_id       = pkt.session_id;
        client_id_bits   = pkt.client_id;
        if (pkt.has_custom_ext) {
            char_id = pkt.char_id;
            token   = pkt.token;
        }
    }

    /// Build a template HandshakePacket for the build function
    ue5_hs::HandshakePacket as_template() const {
        ue5_hs::HandshakePacket t;
        t.magic_header    = magic_header;
        t.session_id      = session_id;
        t.client_id       = client_id_bits;
        t.min_version     = min_version;
        t.cur_version     = cur_version;
        t.network_version = network_version;
        t.network_features= network_features;
        return t;
    }
};

// ─── S>C packet-prefix writer ───────────────────────────────────────────────
//
// Writes the bits that every server→client packet starts with:
//   - Outer layer (38 bits): magic_header(32) + session_id(2) + client_id(3) + handshake_bit(1)
//   - FNetPacketNotify packed header (32 bits) + history word(s) (32 × hist_word_count bits)
//   - Optional AoC custom field (48 bits) if cs.custom_field_captured
//
// Does NOT write the PacketInfo suffix — that differs between synthesized
// packets (fixed bHasPktInfo=1, jitter=1023, bHasFrameTime=0) and replay-
// pass-through (values copied from the captured packet).  Each caller writes
// its own PacketInfo after calling this.
inline void write_sc_packet_prefix(uint8_t* buf, size_t cap, size_t& off,
                                    const ClientState& cs,
                                    bool handshake_bit,
                                    uint16_t hist_word_count = 1) {
    ue5::write_bits(buf, cap, off, cs.magic_header,     32);
    ue5::write_bits(buf, cap, off, cs.session_id,        2);
    ue5::write_bits(buf, cap, off, cs.client_id_bits,    3);
    ue5::write_bits(buf, cap, off, handshake_bit ? 1u : 0u, 1);

    if (hist_word_count < 1) hist_word_count = 1;
    uint32_t packed = (static_cast<uint32_t>(cs.out_seq    & 0x3FFF) << 18) |
                      (static_cast<uint32_t>(cs.in_ack_seq & 0x3FFF) <<  4) |
                      (static_cast<uint32_t>((hist_word_count - 1) & 0x0F));
    ue5::write_bits(buf, cap, off, packed, 32);
    for (uint16_t i = 0; i < hist_word_count; ++i)
        ue5::write_bits(buf, cap, off, 0xFFFFFFFFu, 32);

    if (cs.custom_field_captured) {
        for (int i = 0; i < ClientState::CUSTOM_FIELD_BYTES; ++i)
            ue5::write_bits(buf, cap, off, cs.custom_field[i], 8);
    }
}

// ─── GameServer class ───────────────────────────────────────────────────────

class GameServer : public aoc::net::IGameServerHost {
public:
    struct Config {
        std::string bind_ip          = "0.0.0.0";
        int         port             = 7229;
        std::string relay_target     = "";   // "host:port" for relay mode
        std::string replay_file      = "";   // path to replay_data.bin
        uint32_t    replay_max_packets = 0;  // 0 = all; else cap (bootstrap-only tests)
        // Phase 1 of Protocol module: when true, ignore replay_file and load
        // the bootstrap packets from embedded C++ data
        // (protocol/bootstrap/bootstrap_data.h).  The server binary no
        // longer needs replay_data.bin on disk.  The 400 embedded packets
        // are the same bootstrap subset that already spawns a player into
        // the world; behaviour is bit-identical to --replay-max-packets 400.
        bool        use_embedded_bootstrap = false;
        std::string generator_config = "";   // path to generated_channels.json (Phase D1)
        // Verbose bunch logging — when true, every S>C packet is parsed with
        // aoc::parse_sc_bunch and each bunch is logged in human-readable form.
        // Off by default (very chatty; useful when debugging live traffic).
        bool        verbose_bunches  = false;
        // Limit: stop verbose bunch logging after this many bunches to avoid
        // drowning the log on a full replay.  0 means unlimited.
        uint32_t    verbose_bunch_limit = 200;
        // Skip the first N replay packets before logging starts — lets you
        // sample mid-session traffic instead of the always-same prefix.
        uint32_t    verbose_bunch_start = 0;
        // Optional dedicated output file for [SEND] lines.  Empty → go to
        // the main emu log via spdlog.  Non-empty → open this path and
        // write bunch summaries as plain lines (one per bunch).
        std::string verbose_bunch_log = "";
// -- CoA & C.Y.B --

        // ── Path B: authoritative-server mode ──────────────────────────
        // When true, GameServer starts NativeConnectSequencer after NMT
        // instead of spawning the replay thread.  The sequencer walks a
        // state machine (AwaitNmtJoin → SendBootstrap → SendPcOpen →
        // SendPcProps → Maintain) emitting every packet natively from
        // server-side state.  No captured bytes are played.
        //
        // M1.0 (2026-04-24) wires the scaffolding; handlers are stubs.
        // M1.1-M1.4 progressively fill in real emission.
        bool        native_mode = false;

        // ── Road A — Phase B.0 (2026-04-26) ────────────────────────────
        // When true (and --native is set), GameServer does NOT launch the
        // legacy replay_loop thread.  The replay file is still loaded
        // (so WorldBootstrapEmitter's Splice plan rows can pull captured
        // bytes from it via send_captured_packet), but no parallel thread
        // emits packets — the NativeConnectSequencer is the SOLE driver
        // of the post-NMT bootstrap stream.
        //
        // Why a separate flag (and not just `--replay-max-packets 0`):
        // legacy semantics treat replay_max_packets=0 as "unlimited" (all
        // 29010 captured packets), not "send zero".  Repurposing 0 would
        // break callers that already use the legacy meaning.
        //
        // Pure-native = `--native --replay <file> --no-replay-loop`.
        bool        disable_replay_loop = false;

        // The character name the server uses when emitting the
        // PlayerController's Name property in native mode.  Ignored in
        // replay mode (replay sends the captured name verbatim).
        std::string custom_name = "";

        // ── Path X / V3 — synthetic property update emit (2026-04-26) ───
        // After the captured replay finishes (~100 packets), optionally
        // emit a synthetic property-update bunch to override OUR character's
        // in-game properties (level, HP, name, etc.).  Uses V3 content-block
        // wire format from PropertyUpdateBunchBuilder.
        //
        // Multiple unknowns (controlled by these knobs) — find correct
        // values via in-game iteration:
        //   - which channel hosts our PC (probably 3, may differ)
        //   - NumProperties for the actor's class (try 256 first)
        //   - cmd_handle per property (try common values like 28, 0x6A,
        //     iterate 0..30 if those don't work)
        //
        // Set v3_emit_enabled=true to fire the synthetic bunch after replay.
        bool      v3_emit_enabled        = false;

        // V3 can use a DIFFERENT name than custom_name. Empty = use custom_name.
        std::string v3_custom_name        = "";

        uint32_t  v3_target_channel       = 3;        // PC's actor channel
        // V3 subobject targeting (verified empirical RE 2026-04-26):
        //   0      = target the channel's main actor (bIsChannelActor=1)
        //   non-0  = target a subobject by SIP NetGUID (bIsChannelActor=0)
        // For ch=3 PC, capture decode shows subobj GUID 7193 receives the
        // largest property payloads (up to 2755 bits) — almost certainly
        // PlayerState (where Name lives).
        uint32_t  v3_subobject_guid       = 0;        // 0 = channel root
        uint32_t  v3_num_properties       = 256;      // ceil(log2) for cmd_handle width
        bool      v3_reliable             = false;    // last attempt: false=client applied (wrong prop)
        bool      v3_has_mbg              = true;     // bHasMustBeMappedGUIDs (empirical: capture always sets)
        // bIsReplicationPaused override.  -1 = auto (set 1 when targeting a
        // subobject, 0 otherwise — matches captured ch=3 GUID-7193 pattern).
        // 0 = always send rp=0.  1 = always send rp=1.
        int32_t   v3_rep_paused_override  = -1;

        // V3 test mode — when non-empty, OVERRIDES the per-property emit
        // logic to send a single specific test property.  Used to isolate
        // whether V3 wire format works at all, separate from the question
        // of whether Name updates trigger visible HUD change.
        //   ""          (default) — use existing v3_cmd_*/custom_* emit logic
        //   "spectator" — send bIsSpectator=true at cmd_handle=3 (1-bit bool).
        //                 If V3 works, HUD vanishes and player switches to
        //                 spectator mode.  HIGHLY VISIBLE.
        //   "score"     — send Score=999 at cmd_handle=1 (32-bit int32).
        //                 May or may not be visible in AOC HUD.
        //   "pvpoptin"  — send bRemovePVPOptInProtection=true at cmd_handle=1
        //                 TARGETING UAoCStatsComponent (= GUID 7193, NOW VERIFIED).
        //                 Tests modern inner format on the CORRECT class.
        std::string v3_test_mode = "";

        // V3 inner-property wire format selector (2026-04-26 finding):
        //   true  (default) — modern: [SerializeInt handle][value bits]
        //   false           — legacy: [SerializeInt handle][SIP NumBits][value]
        // Per RE of sub_143F2DC60: modern path L169-220 doesn't read NumBits.
        bool        v3_use_modern_format = true;
        uint32_t  v3_cmd_handle_name      = 0x6A;     // observed in pkt#104 (Name slot)
        uint32_t  v3_cmd_handle_level     = 28;       // catalog guess — iterate
        uint32_t  v3_cmd_handle_health    = 0;         // unknown — iterate
        uint32_t  v3_cmd_handle_max_health= 0;
        uint32_t  v3_cmd_handle_gold      = 0;

        // Which V3 properties to actually emit (use -1 in custom_* to skip).
        // E.g. setting v3_emit_enabled=true with custom_level=25 will emit
        // a Level=25 update; if custom_level=-1, level is skipped.
// [C O A x C | Y | B]

        // ── Tier-1 property patches (2026-04-25) ───────────────────────
        // Fixed-width property overrides applied to replay packets at load
        // time.  Each field below has a "captured value" we observed in
        // replay_data.bin when the capture was recorded.  Setting a
        // non-default value here causes the patcher to find those bytes
        // in replay packets and overwrite them with the new value.
        //
        // ZERO RISK: fixed-width overwrites don't change packet size or
        // bit alignment, so the client's parser is unaffected.
        //
        // Captured values below come from the Cleric (class_id=17748)
        // character that was used to record replay_data.bin.
        //
        // Leave as the sentinel (see defaults) to skip patching that field.
        // Set to a new value to apply the patch.
        int32_t  custom_level         = -1;   // -1 = skip; set 1-60 to patch
        int32_t  custom_hp_current    = -1;   // -1 = skip
        int32_t  custom_hp_max        = -1;   // -1 = skip
        int32_t  custom_mp_current    = -1;   // -1 = skip
        int32_t  custom_mp_max        = -1;   // -1 = skip
        int32_t  custom_stamina_max   = -1;   // -1 = skip
        int32_t  custom_gold          = -1;   // -1 = skip
        int32_t  custom_xp_current    = -1;   // -1 = skip
        int32_t  custom_str           = -1;   // -1 = skip
        int32_t  custom_dex           = -1;   // -1 = skip
        int32_t  custom_int_stat      = -1;   // -1 = skip (renamed to avoid 'int' keyword)
        int32_t  custom_vit           = -1;   // -1 = skip
        int32_t  custom_class_id      = -1;   // -1 = skip (visible Cleric=17748 captured)
        int32_t  custom_race_id       = -1;   // -1 = skip

        // Captured values observed in replay_data.bin, in-game HUD 2026-04-25:
        //   Level=1, HP=90/90, MP=90/90, Stamina=100, Class=Cleric(17748).
        // The patcher uses these as the "needle" to find the live value slots.
        // Override these if you recorded a fresh replay with different state.
        //
        // NOTE: Level=1 and stat=10 are generic values — the patcher will
        // SAFETY ABORT any rule matching more than max_safe_hits=10 times.
        // Required fix: anchored patterns (Tier-1.5 — see SESSION-END-STATE.md).
        int32_t  captured_level       = 1;       // HUD confirmed
        int32_t  captured_hp_current  = 90;      // HUD confirmed (not 100)
        int32_t  captured_hp_max      = 90;      // HUD confirmed (not 100)
        int32_t  captured_mp_current  = 90;      // HUD confirmed (not 100)
        int32_t  captured_mp_max      = 90;      // HUD confirmed (not 100)
        int32_t  captured_stamina_max = 100;     // HUD confirmed
        int32_t  captured_gold        = 0;       // guess — needs verification
        int32_t  captured_xp_current  = 0;       // guess — needs verification
        int32_t  captured_str         = 10;      // guess — needs verification
        int32_t  captured_dex         = 10;      // guess — needs verification
        int32_t  captured_int_stat    = 10;      // guess — needs verification
        int32_t  captured_vit         = 10;      // guess — needs verification
        int32_t  captured_class_id    = 17748;   // Cleric (confirmed from existing logs)
        int32_t  captured_race_id     = -1;      // unknown — disable by default

        // ── Session F: live-world integration ─────────────────────────
        // When true, GameServer spins up a LiveWorld alongside the legacy
        // handshake/replay path.  LiveWorld owns the Session/World/Emit
        // layers end-to-end: every post-handshake packet is ALSO fed to
        // the OpcodeDispatcher, and every connected client appears in the
        // SessionRegistry.  The legacy replay loop still runs — it's the
        // source of byte-perfect captured bunches — but the new pipeline
        // is live and observable, ready to take over in Session G.
        bool        enable_live_world = false;
        // Replication-tick frequency for the LiveWorld broadcast manager.
        // Independent of simulation_hz per architectural correction 3.
        uint32_t    live_world_replication_hz = 20;

        // ── Session G: active byte-level emission ─────────────────────
        // When enable_live_world is true, Session G's UdpPacketEmitter
        // always PRODUCES complete UE5 bytes per emit.  `session_g_send`
        // controls whether those bytes actually hit the socket:
        //   false (default, safe):  LOG bytes + hex preview, drop them.
        //                            Client is unaffected; you can inspect
        //                            the output for structural correctness.
        //   true  (active):          sendto() the bytes alongside legacy
        //                            replay traffic.  Use with caution —
        //                            may collide with replay seq/ack until
        //                            Session H retires the replay path.
        bool        session_g_send = false;

        // When enable_live_world is true and a client finishes handshake,
        // spawn the player's PC/Pawn/PlayerState actors in LiveWorld's
        // ActorRegistry.  Default: on.  Turn off to keep LiveWorld in pure
        // Session F observer mode even with --enable-live-world.
        bool        session_g_spawn_actors = true;

        // Answer ServerUpdateLevelVisibility with ClientAckUpdateLevelVisibility
        // (--send-level-visibility-acks).  Off by default: levels became
        // visible without acks in every captured run.
        bool        send_level_visibility_acks = false;

        // Answer RequestHitVerification_Server with ClientHitConfirmationsReliable
        // (--send-hit-confirmations).  Off by default: the reply's envelope and
        // blob layout come from the client binary, but its GUIDs are placeholders
        // until the request is decoded properly, and targets are not encoded.
        bool        send_hit_confirmations = false;

        // Answer ServerCheckClientPossession(Reliable) (PC fields 77/78) with
        // ClientRestart(Pawn) (field 45), at most 3 times per session
        // (--answer-possession-checks).  Off by default until a live run shows
        // the client replying ServerAcknowledgePossession (field 73).
        bool        answer_possession_checks = false;

        // (Session H.1 `allow_variable_name` was removed — patcher gone.)
        // (Phase II mutate_disable_pkt* fields were removed 2026-04-23 —
        //  ReplayMutator itself was never activated in the live path.)

        // ── Session H.4: live PC spawn via our emitter ─────────────────
        // When true, after the replay is loaded we OVERWRITE the PC ActorOpen
        // bunch bits in replay_data_->packets[22] with output from our own
        // ActorBuilder + PackageMapExporter (with spliced RepLayout tail from
        // the captured bunch itself).
        //
        // Our emitter produces bit-identical output to captured through the
        // full 4859-bit bunch, verified by test_pc_spawn_diff (100.0% match).
        // The splice pulls the 848-bit RepLayout tail directly from the
        // captured bunch so final bits stay byte-identical.
        //
        // Removed: live_pc_spawn / live_pawn_spawn.
        // Their attempt to re-emit single fragments of multi-packet
        // logical bunches was conceptually flawed.  Phase II replaces
        // them with spawn_player_controller_for_client() which builds
        // the FULL logical ActorOpen from scratch.
    };

    explicit GameServer(Config config = {})
        : config_(std::move(config)) {
        // Generate HMAC secret for cookie generation
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : hmac_secret_)
            b = static_cast<uint8_t>(dist(gen));

        // Load replay data.  Two mutually-exclusive sources:
        //  1. --use-embedded-bootstrap: compiled-in 400-packet bootstrap.
        //     The server has no external file dependency.
        //  2. --replay <path>: captured packet file (legacy / research path).
        // If both are set, embedded wins (preferred for "no-file" deployment).
        if (config_.use_embedded_bootstrap) {
            replay_data_ = std::make_unique<ReplayData>();
            if (!aoc::protocol::BootstrapSequence::fill(*replay_data_)) {
                spdlog::error("[GameServer] Embedded bootstrap failed to load");
                replay_data_.reset();
            } else {
                // Phase II Stage 2.0 — DRY-RUN framing validation.
                spdlog::info("[GameServer] Running Stage 2.0 dry-run "
                             "synthesis round-trip test ...");
                const bool roundtrip_ok =
                    aoc::protocol::BootstrapSequence::test_pc_spawn_round_trip(
                        *replay_data_);
                // Phase II Stage 2.1 — SPLICE synthesis into replay stream.
                // Only runs if the dry-run test validated bit-identical;
                // otherwise the splice would be a no-op or corruption risk.
                // With identical bytes this is a functional no-op, but it
                // wires the integration path for future divergence.
                if (roundtrip_ok) {
                    spdlog::info("[GameServer] Stage 2.1: splicing "
                                 "synthesized PC spawn bytes into "
                                 "pkt#22.raw (byte-identical, no "
                                 "functional change; infrastructure "
                                 "for later divergence).");
                    aoc::protocol::BootstrapSequence::splice_pc_spawn_synthesis(
                        *replay_data_);
                }
            }
        } else if (!config_.replay_file.empty()) {
            replay_data_ = std::make_unique<ReplayData>();
            if (!replay_data_->load(config_.replay_file)) {
                spdlog::error("[GameServer] Failed to load replay, falling back to emulation");
                replay_data_.reset();
            }
        }
// (C O A + Cyb)

        // ── Tier-1 property patcher (2026-04-25) — run ONCE at startup ─
        // Regardless of source (embedded bootstrap OR external replay file),
        // replay_data_ is populated above.  Apply fixed-width property
        // overrides NOW, before any client connects.  Runs once per server
        // lifetime.  Each patch is a SAME-SIZE byte overwrite (int32→int32,
        // float→float) so packet structure and bit alignment are preserved.
        if (replay_data_) {
            using aoc::protocol::tools::ReplayPropertyPatcher;
            ReplayPropertyPatcher patcher;
            auto add_i32 = [&](const char* name, int32_t cap, int32_t newv) {
                if (newv >= 0 && newv != cap) {
                    patcher.add_int32(name, cap, newv);
                }
            };
            add_i32("character_level",    config_.captured_level,       config_.custom_level);
            add_i32("character_hp_curr",  config_.captured_hp_current,  config_.custom_hp_current);
            add_i32("character_hp_max",   config_.captured_hp_max,      config_.custom_hp_max);
            add_i32("character_mp_curr",  config_.captured_mp_current,  config_.custom_mp_current);
            add_i32("character_mp_max",   config_.captured_mp_max,      config_.custom_mp_max);
            add_i32("character_stamina",  config_.captured_stamina_max, config_.custom_stamina_max);
            add_i32("character_gold",     config_.captured_gold,        config_.custom_gold);
            add_i32("character_xp_curr",  config_.captured_xp_current,  config_.custom_xp_current);
            add_i32("character_str",      config_.captured_str,         config_.custom_str);
            add_i32("character_dex",      config_.captured_dex,         config_.custom_dex);
            add_i32("character_int",      config_.captured_int_stat,    config_.custom_int_stat);
            add_i32("character_vit",      config_.captured_vit,         config_.custom_vit);
            add_i32("character_class_id", config_.captured_class_id,    config_.custom_class_id);
            if (config_.captured_race_id >= 0) {
                add_i32("character_race_id", config_.captured_race_id, config_.custom_race_id);
            }
            if (patcher.enabled_rule_count() > 0) {
                // Tier1 is the M2.1-era byte-level replay patcher. It tries
                // to find captured int32 values (HP=100 etc) in raw replay
                // bytes and substitute them. Most stats aren't byte-aligned
                // so it normally finds 0 matches.  Logs lowered from warn
                // to debug 2026-04-26 — they were drowning the console with
                // expected "0 matches" reports for fields that simply aren't
                // patchable via byte search.  Set spdlog level to debug
                // (or use --verbose-bunches) to see them.
                spdlog::debug("[GameServer][Tier1] applying {} property patch "
                              "rule(s) to {} packets at startup...",
                              patcher.enabled_rule_count(),
                              replay_data_->packets.size());
                auto reports = patcher.apply_all(replay_data_->packets);
                int n_applied = 0;
                int n_aborted = 0;
                for (const auto& rep : reports) {
                    if (rep.applied_count > 0) ++n_applied;
                    else ++n_aborted;
                    spdlog::debug("[GameServer][Tier1] '{}' applied={}",
                                  rep.name, rep.applied_count);
                }
                if (n_applied > 0) {
                    spdlog::info("[GameServer][Tier1] {} rule(s) applied, "
                                 "{} skipped (captured value unmatched or "
                                 "too generic). Run with --verbose-bunches "
                                 "for per-rule detail.", n_applied, n_aborted);
                }
                // Silent on n_applied==0 — that's the common case and not
                // actionable noise.
            }
        }
        // (allow_variable_name propagation removed — patcher is gone.)

        // Phase D1: load bunch-generator allowlist (optional).
        if (!config_.generator_config.empty()) {
            load_generator_config(config_.generator_config);
        }

        // Session F: stand up the live-world shim if requested.  Schema
        // registry is populated once (idempotent) — ActorBuilder inside
        // LiveWorld's UDP emitter needs it.
        //
        // Session G adds two callbacks:
        //   outer_fn: on each emit, LiveWorld asks us for the client's
        //             outer-packet state (magic header, sess/client ids,
        //             seq counters, custom field).  We snapshot it under
        //             client_mu_ and bump out_seq so concurrent emits don't
        //             reuse the same sequence number.
        //   send_fn:  actually dispatch the completed byte buffer.  In
        //             dry-run mode this just logs + counts; in active-send
        //             mode it forwards to the real sendto via send_to_client.
        if (config_.enable_live_world) {
            auto& schemas = aoc::protocol::schema::SchemaRegistry::instance();
            schemas.load_all();

            aoc::net::LiveWorldConfig lwc;
            lwc.replication_hz = config_.live_world_replication_hz;

            // Per-emit outer-packet state snapshot.  Critical contract:
            //   - DRY-RUN (config_.session_g_send=false): PEEK ONLY.  Do
            //     NOT bump cs.out_seq / cs.reliable_seq, because the legacy
            //     replay path owns those counters and the client is being
            //     driven by the replay's bytes.  Bumping here desyncs the
            //     legacy path and breaks handshake (observed 18:59 run).
            //   - ACTIVE-SEND (session_g_send=true): advance, because our
            //     bytes are actually going on the wire and would collide
            //     with the legacy path otherwise (that's the session_g_send
            //     caveat we already document in Config).
            auto outer_fn = [this](const std::string& client_key)
                -> std::optional<aoc::net::OuterPacketState> {
                std::lock_guard<std::mutex> lk(client_mu_);
                auto it = clients_.find(client_key);
                if (it == clients_.end()) return std::nullopt;
                auto& cs = it->second;
                if (cs.phase < ClientState::HANDSHAKE_COMPLETE) return std::nullopt;
                aoc::net::OuterPacketState s;
                s.magic_header        = cs.magic_header;
                s.session_id          = cs.session_id;
                s.client_id_bits      = cs.client_id_bits;
                s.out_seq             = cs.out_seq;
                s.in_ack_seq          = cs.in_ack_seq;
                s.reliable_ch_seq     = cs.reliable_seq;
                s.custom_field_present = cs.custom_field_captured;
                if (cs.custom_field_captured) {
                    std::memcpy(s.custom_field, cs.custom_field, 6);
                }
                s.reserve_bytes_hint  = 512;
                if (config_.session_g_send) {
                    // Only advance counters when we're actually transmitting.
                    cs.out_seq      = (cs.out_seq + 1) & ClientState::SEQ_MASK;
                    cs.reliable_seq = cs.reliable_seq + 1;
                }
                return s;
            };

            auto send_fn = [this](const std::string& client_key,
                                    const uint8_t* data, size_t len) {
                // Session G safety: default is dry-run.  When
                // --session-g-send is off, we only LOG the byte count + a
                // hex preview; the client never sees these packets.
                if (!config_.session_g_send) {
                    spdlog::info("[SessionG/dry-run] would send {}B to {} "
                                  "(first 16B: {})",
                                  len, client_key,
                                  ::ue5::hex_dump(data, std::min<size_t>(len, 16), 16));
                    return;
                }
                sockaddr_in addr{};
                {
                    std::lock_guard<std::mutex> lk(client_mu_);
                    auto it = clients_.find(client_key);
                    if (it == clients_.end()) return;
                    addr = it->second.addr;
                }
                int sent = send_to_client(data, len, addr);
                if (sent <= 0) {
                    spdlog::warn("[SessionG] send_to_client failed for {}: len={} sent={}",
                                  client_key, len, sent);
                }
            };
// [C.O.A - Cyb]

            live_world_ = std::make_unique<aoc::net::LiveWorld>(
                schemas, std::move(outer_fn), std::move(send_fn), lwc);
        }
    }

    ~GameServer() { stop(); }

    bool is_relay_mode() const { return !config_.relay_target.empty() || relay_active_.load(); }

    /// Dynamically enable relay mode (called when a PlayReply is intercepted).
    /// Resolves the target hostname and creates the upstream socket on the fly.
    /// SMALL STEP 1: link to XClient's character store.  The callback is
    /// invoked right before the replay loop starts; returning a non-empty
    /// string patches the captured replay with that name in memory.
    void set_character_name_provider(std::function<std::string()> fn) {
        character_name_provider_ = std::move(fn);
    }

    /// Optional: provider returning the current player's archetype_id (class).
    /// Invoked right before the replay loop starts.  When non-zero, the
    /// captured archetype in pkt[22] is overwritten so the HUD + character
    /// portrait show the user's chosen class instead of RandomChar (Cleric).
    void set_character_archetype_provider(std::function<uint64_t()> fn) {
        character_archetype_provider_ = std::move(fn);
    }

    /// PD2.3 (2026-05-05) — Optional: provider returning the customization
    /// JSON blob (~5 KB) for the currently selected character.  Used by
    /// AppearanceEmitter to populate FCharacterCustomizationSaveData fields
    /// from real per-character data instead of zero defaults.
    /// When unset or returning empty string, AppearanceEmitter falls back
    /// to defaults (which produces an empty mesh).
    void set_customization_json_provider(std::function<std::string()> fn) {
        customization_json_provider_ = std::move(fn);
    }

    /// PD2.3 — Read-side accessor used by AppearanceEmitter via IGameServerHost.
    std::string current_character_customization_json() const {
        return customization_json_provider_ ? customization_json_provider_()
                                             : std::string{};
    }

    /// PD3 (2026-05-05) — Read-side accessor for the currently-selected
    /// character name.  Reuses the existing character_name_provider_
    /// (already wired to xclient_service.last_character_name() in main.cpp).
    std::string current_character_name() const override {
        return character_name_provider_ ? character_name_provider_()
                                         : std::string{};
    }

    bool emit_client_initialize_character(
            const std::string& client_key,
            const sockaddr_in& addr) override {
        // The internal helper signature still takes a character_name string
        // (legacy from PM28) but the body ignores it.  Pass empty string.
        return send_client_initialize_character_native(client_key, addr,
                                                        std::string{});
    }

    bool enable_relay(const std::string& target) {
        if (relay_active_.load()) {
            spdlog::info("[GameServer] Relay already active, updating target: {}", target);
        }
        config_.relay_target = target;
        if (!resolve_relay_target()) {
            spdlog::error("[GameServer] Failed to resolve relay target '{}'", target);
            return false;
        }
        // Create upstream socket if needed
        if (upstream_sock_ == INVALID_SOCK) {
            upstream_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (upstream_sock_ == INVALID_SOCK) {
                spdlog::error("[GameServer] Failed to create upstream socket for relay");
                return false;
            }
#ifdef _WIN32
            DWORD timeout_ms = 100;
            setsockopt(upstream_sock_, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
            struct timeval tv{0, 100000};
            setsockopt(upstream_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        }
        relay_active_ = true;
        // Start upstream reader thread if not already running
        if (!upstream_thread_.joinable()) {
            upstream_thread_ = std::thread([this]() { upstream_loop(); });
        }
        spdlog::warn("[GameServer] ★ RELAY ACTIVATED: {} → {}", config_.port, target);
        return true;
    }

    bool start() {
        if (running_) return true;
        init_sockets();

        if (is_relay_mode()) {
            if (!resolve_relay_target()) return false;
        }

        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCK) {
            spdlog::error("[GameServer] Failed to create UDP socket");
            return false;
        }

        int opt = 1;
#ifdef _WIN32
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
        DWORD timeout_ms = 100;  // 100ms for fast keepalive loop
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct timeval tv{0, 100000};  // 100ms for fast keepalive loop
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(config_.port));
        inet_pton(AF_INET, config_.bind_ip.c_str(), &addr.sin_addr);

        if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            spdlog::error("[GameServer] Failed to bind on {}:{}", config_.bind_ip, config_.port);
            close_socket(sock_);
            sock_ = INVALID_SOCK;
            return false;
        }

        // Relay mode upstream socket
        if (is_relay_mode()) {
            upstream_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (upstream_sock_ == INVALID_SOCK) {
                spdlog::error("[GameServer] Failed to create upstream socket");
                close_socket(sock_);
                sock_ = INVALID_SOCK;
                return false;
            }
#ifdef _WIN32
            setsockopt(upstream_sock_, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
            setsockopt(upstream_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        }
// C-Y-B + C.O.A

        start_time_ = std::chrono::steady_clock::now();
        running_ = true;

        // Session F: bring up the live-world tick thread.  Idempotent —
        // safe to call even if live_world_ is nullptr.
        if (live_world_) live_world_->start();

        thread_ = std::thread([this]() { run_loop(); });

        if (is_relay_mode()) {
            upstream_thread_ = std::thread([this]() { upstream_loop(); });
            spdlog::info("[GameServer] UDP RELAY on {}:{} -> {}", config_.bind_ip, config_.port, config_.relay_target);
        } else {
            spdlog::info("[GameServer] UDP EMULATION on {}:{} [UE5 handshake v{}-v{}]",
                         config_.bind_ip, config_.port,
                         static_cast<int>(ue5_hs::SessionClientId),
                         static_cast<int>(ue5_hs::Latest));
        }
        return true;
    }

    void stop() {
        running_ = false;
        // Session F: stop the live-world tick thread BEFORE closing sockets
        // so its final tick doesn't race a send against a closed fd.
        if (live_world_) live_world_->stop();
        if (sock_ != INVALID_SOCK) { close_socket(sock_); sock_ = INVALID_SOCK; }
        if (upstream_sock_ != INVALID_SOCK) { close_socket(upstream_sock_); upstream_sock_ = INVALID_SOCK; }
        if (thread_.joinable()) thread_.join();
        if (upstream_thread_.joinable()) upstream_thread_.join();
        if (replay_thread_.joinable()) replay_thread_.join();
        if (bootstrap_thread_.joinable()) bootstrap_thread_.join();
        // PHASE B: flush verbose bunch stats before destruction.
        flush_verbose_bunch_summary();
    }

    // ── Session F diagnostic accessor (optional; null when live-world off) ─
    aoc::net::LiveWorld* live_world() { return live_world_.get(); }

private:
    Config      config_;
    socket_t    sock_          = INVALID_SOCK;
    socket_t    upstream_sock_ = INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::atomic<bool> relay_active_{false};

    // SMALL STEP 1: pulled from XClient at replay-start time, returns the
    // live player's chosen character name (empty if none).
    std::function<std::string()> character_name_provider_;
    std::function<uint64_t()>    character_archetype_provider_;
    // PD2.3: optional provider for the current character's customization JSON
    std::function<std::string()> customization_json_provider_;

    // ── Session 3: Multiplayer NetGUID allocator (shared across all
    //    connected clients).  Each player's actors are assigned a unique
    //    block of 256 GUIDs starting at 0x01000000.  See
    //    protocol/net_guid_allocator.h for design notes.
    aoc::protocol::NetGuidAllocator net_guid_allocator_;
    std::thread thread_;
    std::thread upstream_thread_;
    sockaddr_in upstream_addr_{};
    std::chrono::steady_clock::time_point start_time_;

    // Client tracking
    std::mutex client_mu_;
    std::unordered_map<std::string, ClientState> clients_;
    sockaddr_in last_client_addr_{};
    bool        client_known_ = false;

    // Server secret for cookie generation
    std::array<uint8_t, ue5_hs::SECRET_SIZE> hmac_secret_{};

    // Replay data (loaded from binary file)
    std::unique_ptr<ReplayData> replay_data_;
    std::thread replay_thread_;

    // Path B: authoritative-server sequencer.  Instantiated after NMT when
    // config_.native_mode is set (instead of replay_thread_).
    std::unique_ptr<aoc::net::NativeConnectSequencer> native_sequencer_;

    // ── Session F: live-world integration (optional) ────────────────────
    // Null unless config_.enable_live_world was set at construction time.
    // When present: receives a copy of every post-handshake packet via
    // maybe_feed_live_world(); runs its own replication-tick thread to
    // fan state changes out to connected clients.
    std::unique_ptr<aoc::net::LiveWorld> live_world_;

    // Phase D1: registered bunch generators (one per allowlisted channel).
    // The outbound replay loop ticks each entry once per iteration.  Empty
    // unless config_.generator_config was loaded at startup.
    std::vector<std::unique_ptr<aoc::BunchGenerator>> generators_;
    uint64_t gen_tick_counter_ = 0;
    std::chrono::steady_clock::time_point gen_session_start_ =
        std::chrono::steady_clock::now();
    std::atomic<bool> replay_active_{false};    // true once replay_loop started
    std::atomic<bool> replay_map_loaded_{false}; // true when client finished LoadMap

    // M1.4.d — Fix #36 equivalent for --native mode.  Same semantics as
    // replay_map_loaded_ but for the NativeConnectSequencer path.  The
    // client sends NMT_GameSpecific BEFORE its game thread finishes
    // LoadMap(); if we emit PC ActorOpen immediately, those bunches are
    // dropped while the client is still loading → player spawns
    // underwater at world-origin with an empty world.  Sequencer waits
    // on this flag before entering SendBootstrap/SendPcOpen.  Set by
    // the data-receive path when client sends first non-empty bunch
    // post-NMT (state >= 4).
    std::atomic<bool> native_map_loaded_{false};

    // Phase A.3 (2026-04-26) — server-minted NetGUID block allocator for
    // native --native mode.  Per-client allocation is idempotent: the
    // first allocate_player_block(client_key) call mints a fresh block
    // from a monotonic counter (base ≥ 0x01000000, well clear of any
    // captured GUIDs); subsequent calls return the same block.  Used by
    // PcEmitter (and eventually PawnEmitter) to replace the captured PC
    // NetGUID 10341530 with a server-minted GUID, so we can incrementally
    // retire the captured fixtures.
    //
    // This is independent of LiveWorld's own allocator (which only exists
    // when Session G is enabled) so the native sequencer can run without
    // needing the full LiveWorld pipeline.
    aoc::protocol::NetGuidAllocator native_pc_allocator_;
// built by Cyb with C.O.A

    // Bootstrap sender (embedded data from bootstrap_data.h)
    // One thread at a time; per-client guard is ClientState::bootstrap_active.
    std::thread bootstrap_thread_;

    // Packet counters
    std::atomic<uint64_t> pkt_c2s_{0};
    std::atomic<uint64_t> pkt_s2c_{0};

    // PHASE 2c: verbose bunch logging state.
    //   - verbose_logged_bunches_  : capped counter of bunches logged so far
    //   - verbose_bunch_log_stream_: optional dedicated file sink (opened
    //     lazily on first use).  When config_.verbose_bunch_log is empty,
    //     the stream stays closed and output goes to the main emu log.
    uint32_t verbose_logged_bunches_ = 0;
    uint32_t verbose_seen_packets_   = 0; // for --verbose-bunch-start skip
    std::unique_ptr<std::ofstream> verbose_bunch_log_stream_;

    // PHASE B: session aggregates — flushed to the log on shutdown.
    uint32_t verbose_packet_count_  = 0;
    uint32_t verbose_parse_fails_   = 0;
    uint32_t verbose_drift_events_  = 0;
    uint32_t verbose_ghost_events_  = 0;
    std::array<uint32_t, 7> verbose_kind_counts_{};   // one per BunchKind
    std::unordered_map<uint32_t, uint32_t> verbose_channel_counts_;

    // ── Helpers ─────────────────────────────────────────────────────────

    static std::string addr_key(const sockaddr_in& a) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
    }

    /// Session H.1: route a parsed NMT control-channel message into
    /// LiveWorld's OpcodeDispatcher.  No-op if --enable-live-world is off.
    ///
    /// Intentionally LOSSY with respect to payload today — for phase-machine
    /// validation we only need the opcode.  Session H.2 extracts the full
    /// Login URL fields, GameSpecific sub-type, etc. so handlers can emit
    /// real S>C replies.
    void maybe_dispatch_nmt_to_live_world(const std::string& client_key,
                                           uint8_t nmt_type,
                                           const std::vector<uint8_t>& nmt_payload) {
        if (!live_world_) return;

        aoc::net::DispatchPacket dp;
        dp.op         = aoc::net::dispatch_op_for_nmt(nmt_type);
        dp.client_key = client_key;
        dp.payload    = nmt_payload;

        // Populate str_arg so handle_nmt_login's "empty player name" check
        // passes — the full Login URL parse lands in H.2.  For now we feed
        // the XClient character-name provider if available.
        if (dp.op == aoc::net::DispatchOp::NMT_LOGIN) {
            dp.str_arg = character_name_provider_
                ? character_name_provider_() : std::string{"unknown"};
            if (dp.str_arg.empty()) dp.str_arg = "unknown";
        }

        aoc::net::DispatchResult r = live_world_->on_packet(dp);
        if (r.accepted) {
            if (r.phase_changed) {
                spdlog::info("[H.1/dispatch] NMT {} ({}): {} -> {} for {}",
                              nmt_type, aoc::net::nmt_name(nmt_type),
                              aoc::net::to_string(r.old_phase),
                              aoc::net::to_string(r.new_phase),
                              client_key);
            } else {
                spdlog::debug("[H.1/dispatch] NMT {} ({}) accepted (no phase change) for {}",
                               nmt_type, aoc::net::nmt_name(nmt_type), client_key);
            }
        } else {
            // Rejected — most commonly "wrong phase" because of initial
            // race between legacy handshake + LiveWorld registration.
            // Log at debug to avoid log-spam once H.2 resolves this.
            spdlog::debug("[H.1/dispatch] NMT {} ({}) REJECTED for {}: {}",
                           nmt_type, aoc::net::nmt_name(nmt_type), client_key, r.error);
        }
    }

    /// Session G: spawn the player's PC/Pawn/PlayerState actors in
    /// LiveWorld's ActorRegistry so the BroadcastManager has real state
    /// to replicate.  Pulls identity from the optional character_name /
    /// archetype providers the launcher wires up; falls back to reasonable
    /// defaults when none are set.
    ///
    /// Called under no lock; the registry itself is thread-safe.  NetGUIDs
    /// come from LiveWorld's per-session allocator so they don't collide
    /// with the legacy replay's hardcoded NetGUIDs (which live below
    /// 0x01000000 in captures vs. ≥0x01000000 here).
    void spawn_session_g_actors_for(const std::string& client_key) {
        if (!live_world_) return;
        auto* cs = live_world_->sessions().get(client_key);
        if (!cs) {
            spdlog::warn("[SessionG] spawn: no session for {}", client_key);
            return;
        }
// Cyb captures, C O A code

        // Pull identity if provider is wired; empty name → synthetic default.
        std::string name = character_name_provider_
            ? character_name_provider_() : std::string{};
        uint64_t archetype_id = character_archetype_provider_
            ? character_archetype_provider_() : 0;
        if (name.empty()) name = "LiveWorldHero";

        // Allocate a block for this client in LiveWorld's allocator.
        // (Legacy path uses its own separate allocator; Session H consolidates.)
        auto block = live_world_->netguid_allocator()
                               .allocate_player_block(client_key);
        cs->netguid_block          = block;
        cs->pc_netguid             = block.player_controller;
        cs->pawn_netguid           = block.pawn;
        cs->player_state_netguid   = block.player_state;
        cs->player_name            = name;

        namespace sim   = aoc::world::simulation;
        namespace emit  = aoc::protocol::emit;
        namespace sch   = aoc::protocol::schema;

        // ── 1. PlayerController actor ────────────────────────────────
        sim::SimulationActor pc;
        pc.netguid               = block.player_controller;
        pc.type                  = sch::ActorType::PlayerController;
        pc.owner_client_key      = client_key;
        pc.runtime.actor_netguid = block.player_controller;
        // bIsGM handle 3 = false; bIsDev handle 4 = false.  The actual
        // schema (see pc_schema.cpp) defines all 16 root properties.
        pc.set_root(3, emit::SchemaValue::make_bool(false));
        pc.set_root(4, emit::SchemaValue::make_bool(false));
        live_world_->actors().spawn(std::move(pc));

        // ── 2. Pawn actor (with CharacterInformationComponent=2) ─────
        sim::SimulationActor pawn;
        pawn.netguid               = block.pawn;
        pawn.type                  = sch::ActorType::Pawn;
        pawn.owner_client_key      = client_key;
        pawn.runtime.actor_netguid = block.pawn;
        // Component index 2 = CharacterInformationComponent; handle 1 = CharacterName.
        pawn.set_component(2, 1, emit::SchemaValue::make_fstring(name));
        if (archetype_id != 0) {
            // Handle 2 = PrimaryArchetype.
            pawn.set_component(2, 2, emit::SchemaValue::make_u32(
                static_cast<uint32_t>(archetype_id)));
        }
        live_world_->actors().spawn(std::move(pawn));

        // ── 3. PlayerState actor ─────────────────────────────────────
        sim::SimulationActor ps;
        ps.netguid               = block.player_state;
        ps.type                  = sch::ActorType::PlayerState;
        ps.owner_client_key      = client_key;
        ps.runtime.actor_netguid = block.player_state;
        live_world_->actors().spawn(std::move(ps));

        spdlog::info("[SessionG] Spawned PC/Pawn/PS in LiveWorld for {}: "
                      "name=\"{}\" archetype={} pc=0x{:x} pawn=0x{:x} ps=0x{:x}",
                      client_key, name, archetype_id,
                      block.player_controller, block.pawn, block.player_state);
    }

    double elapsed_seconds() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start_time_).count();
    }
// [CoA + C.Y.B]

    /// HMAC-SHA1 cookie generation (matches UE5 StatelessConnectHandlerComponent)
    /// UE5 uses HMAC-SHA1(secret, timestamp || addr) to produce a 20-byte cookie.
    /// The client echoes this cookie back in its Response packet, and the server
    /// re-computes it to verify. Using the same algorithm ensures the client
    /// accepts our Challenge and responds correctly.
    void generate_cookie(double timestamp, const std::string& client_addr,
                         uint8_t* out_cookie) {
        // Build input data: timestamp (8 bytes) + client address string
        uint8_t input[128] = {};
        size_t pos = 0;
        std::memcpy(input + pos, &timestamp, 8); pos += 8;
        size_t addr_len = std::min(client_addr.size(), size_t(64));
        std::memcpy(input + pos, client_addr.data(), addr_len); pos += addr_len;

#ifdef _WIN32
        // Use Windows BCrypt for HMAC-SHA1
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        NTSTATUS status;

        status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM,
                                             nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        if (BCRYPT_SUCCESS(status)) {
            status = BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                                      hmac_secret_.data(),
                                      static_cast<ULONG>(hmac_secret_.size()), 0);
            if (BCRYPT_SUCCESS(status)) {
                BCryptHashData(hHash, input, static_cast<ULONG>(pos), 0);
                BCryptFinishHash(hHash, out_cookie, ue5_hs::COOKIE_SIZE, 0);
                BCryptDestroyHash(hHash);
            }
            BCryptCloseAlgorithmProvider(hAlg, 0);
        }

        if (!BCRYPT_SUCCESS(status)) {
            spdlog::error("[GameServer] BCrypt HMAC-SHA1 failed: 0x{:08X}", status);
            // Fallback: zero cookie (will fail handshake, but won't crash)
            std::memset(out_cookie, 0, ue5_hs::COOKIE_SIZE);
        }
#else
        // POSIX: use OpenSSL HMAC
        unsigned int hmac_len = ue5_hs::COOKIE_SIZE;
        HMAC(EVP_sha1(), hmac_secret_.data(), static_cast<int>(hmac_secret_.size()),
             input, pos, out_cookie, &hmac_len);
#endif
    }

    // ── IGameServerHost interface overrides ─────────────────────────────
    // These let NativeConnectSequencer (in --native mode) send packets,
    // look up client state, and read config values through a narrow
    // contract — so the sequencer file doesn't need to include all of
    // game_server.h.

    int send_to_client(const uint8_t* buf, size_t n,
                        const sockaddr_in& addr) override {
        return send_to_client_impl(buf, n, addr);
    }
    const std::string& custom_name() const override {
        // PM38 (2026-04-30) — prefer the LIVE character name set by the
        // user when they picked a character (XClient on_play tracks the
        // active character_id; provider returns that character's name).
        // Falls back to --custom-name CLI arg only if no character is
        // active yet (early-handshake or test harnesses).
        //
        // The provider returns a fresh std::string each call, so we
        // cache it in a thread-local static to give a stable reference
        // for the duration of this call site.  Callers that need a
        // const std::string& shouldn't outlive the function context.
        if (character_name_provider_) {
            thread_local std::string cached;
            cached = character_name_provider_();
            if (!cached.empty()) return cached;
        }
        return config_.custom_name;
    }
    bool send_keepalive_for(const std::string& client_key,
                              const sockaddr_in& addr) override {
        // Look up ClientState under lock and call the existing send_keepalive
        // helper.  This matches the pattern used by the replay's post-replay
        // keepalive loop (end of replay_loop).
        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(client_key);
        if (it == clients_.end()) return false;
        if (it->second.phase < ClientState::CONNECTED) return false;
        send_keepalive(it->second, addr, client_key);
        return true;
    }

    bool send_bunch_packet(const std::string& client_key,
                             const sockaddr_in& addr,
                             const uint8_t* bunch_data,
                             size_t bunch_bits) override {
        // Build a full UDP packet wrapping the given bunch bits, using the
        // client's current seq/ack state.  Mirrors the keepalive emission
        // path but with a real bunch payload between the PacketInfo and
        // the termination sentinel.
        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(client_key);
        if (it == clients_.end()) return false;
        if (it->second.phase < ClientState::CONNECTED) return false;
        ClientState& cs = it->second;

        uint8_t buf[4096] = {};
        size_t  off = 0;

        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);

        // PacketInfo — hasPacketInfo=1, jitter=max, no ServerFrameTime
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10);
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);

        // Append bunch bits
        for (size_t i = 0; i < bunch_bits; ++i) {
            int bit = (bunch_data[i >> 3] >> (i & 7)) & 1;
            ue5::write_bits(buf, sizeof(buf), off, bit, 1);
        }
// == C.O.A | C | Y | B ==

        // ── EXTRA SENTINEL BIT (AoC PacketHandler requirement) ──
        // Phase B.0p5 (2026-04-27 morning) — bug found via GUID88-SCAN.
        // PawnEmitter splices captured pkt#78 via this function.  Without
        // the sentinel bit, AoC's PacketHandler reports "0's in last byte"
        // 3ms after emission and silently drops the packet — meaning the
        // Pawn ActorOpen never reaches the actor channel.  Same fix as
        // send_keepalive (~5659), send_ch0_reliable_payload (~1781), and
        // send_client_restart_native (~2000).
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);
        uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        int sent = send_to_client_impl(buf, pkt_len, addr);
        if (sent > 0) {
            spdlog::info("[GameServer] send_bunch_packet: seq={} {}B "
                         "({} bunch bits)", sent_seq, pkt_len, bunch_bits);
            // Native emitters send through here; logging the packet lets
            // replay_bunches decode a live run's capture offline.
            ProxyLogger::instance().log_udp("S>C", client_key, buf, pkt_len, "NativeBunch");
            // Phase B.0p4 (2026-04-28 PM) — flag pkt#78 emission so the
            // SNLW reactive handler knows NetGUID 88 is now registered on
            // the client and a native ClientRestart can be safely fired.
            // pkt#78's bunch stream is 4725 bits in v3 (bunch[0]+bunch[2]).
            // PawnEmitter is the only caller that ships exactly that many
            // bits, so use it as a fingerprint.  If pkt#78 size changes
            // again, update this constant.
            constexpr size_t kPkt78BunchBits_v3 = 4725;
            if (bunch_bits == kPkt78BunchBits_v3) {
                cs.pkt78_emitted = true;
                spdlog::info("[GameServer] (pkt#78 emission flagged on '{}' — "
                             "reactive ClientRestart now eligible)", client_key);
            }
            return true;
        }
        spdlog::warn("[GameServer] send_bunch_packet: send failed");
        return false;
    }

    bool send_ch0_reliable_payload(const std::string& client_key,
                                     const sockaddr_in& addr,
                                     const uint8_t* payload,
                                     size_t payload_bytes) override {
        // Build a ch=0 reliable data bunch with the given payload bytes.
        // Mirrors the structure decoded from captured pkt#0 (opcode-3):
        //   [1] bControl=0
        //   [1] bIsReplicationPaused=0
        //   [1] bReliable=1
        //   [SIP] ChIdx = 0
        //   [1] bHasPME=0  [1] bHasMBG=0  [1] bPartial=0
        //   [10] ChSeq (ch=0 uses 10-bit ChSeq for NMT/control)
        //   [1] ChName.bIsHardcoded=1  [SIP] EName=255
        //   [13] BDB = payload_bytes * 8
        //   [N] payload bits
        //
        // Per native-bootstrap-sequence.md §2.1.  This is the foundation
        // call for any ch=0 control opcode we emit natively.
        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(client_key);
        if (it == clients_.end()) return false;
        if (it->second.phase < ClientState::CONNECTED) return false;
        ClientState& cs = it->second;

        // Build the bunch into a temp buffer
        uint8_t bunch_buf[512] = {};
        size_t bb = 0;
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bControl=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bIsReplicationPaused=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 1, 1); // bReliable=1
        ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb, 0);    // ChIdx=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bHasPME=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bHasMBG=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bPartial=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                        cs.reliable_seq & 0x3FF, 10);            // ChSeq (10-bit ch=0)
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 1, 1); // ChName.bIsHardcoded=1
        ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb, 255);  // EName=255 (control name)
        const uint32_t bdb_bits = static_cast<uint32_t>(payload_bytes * 8);
        aoc::protocol::wire::write_bunch_data_bits(bunch_buf, sizeof(bunch_buf), bb, bdb_bits); // BDB
        // Payload bytes
        for (size_t i = 0; i < payload_bytes; ++i) {
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, payload[i], 8);
        }
        const size_t bunch_bits = bb;
// ~ C-O-A - Cyb ~

        // Wrap in UDP packet (same shape as send_bunch_packet)
        uint8_t buf[1024] = {};
        size_t off = 0;
        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);   // bHasPacketInfo
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10);  // jitter=max
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);   // bHasServerFrameTime
        // Splice bunch bits
        for (size_t i = 0; i < bunch_bits; ++i) {
            int bit = (bunch_buf[i >> 3] >> (i & 7)) & 1;
            ue5::write_bits(buf, sizeof(buf), off, bit, 1);
        }
        // ── EXTRA SENTINEL BIT (AoC requirement, see send_keepalive ~5659) ──
        // AoC client's PacketHandler expects an extra '1' bit BEFORE the
        // standard add_termination.  Without it the handler misprocesses
        // and reports "Received packet with 0's in last byte" → fault →
        // DisconnectCountdown.  Splice/captured packets bring this bit in
        // their payload; native packets must add it explicitly.
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);
        const uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;
        const uint16_t sent_chseq = cs.reliable_seq & 0x3FF;
        cs.reliable_seq++;

        int sent = send_to_client_impl(buf, pkt_len, addr);
        if (sent > 0) {
            spdlog::info("[GameServer] >> ch=0 reliable bunch ({} payload B) "
                         "seq={} chSeq={} ({}B total)",
                         payload_bytes, sent_seq, sent_chseq, pkt_len);
            return true;
        }
        spdlog::warn("[GameServer] send_ch0_reliable_payload: send failed");
        return false;
    }

    bool has_client_finished_map_load() const override {
        // M1.4.d — Set by the data-receive path when the client sends its
        // first non-empty bunch post-NMT.  Before this flag goes true,
        // the sequencer should NOT emit any world-data bunches (they'll
        // be dropped while the client's game thread is still in LoadMap).
        return native_map_loaded_.load();
    }

    aoc::protocol::PlayerNetGuidBlock allocate_player_block(
        const std::string& client_key) override {
        // Idempotent — return existing block if already allocated for
        // this client.  This lets PcEmitter, PawnEmitter, and any future
        // emitter share the same block without coordinating allocation
        // order amongst themselves.
        auto existing = native_pc_allocator_.get_block(client_key);
        if (existing.is_valid()) return existing;
        return native_pc_allocator_.allocate_player_block(client_key);
    }

    /// Walk an OUTGOING (S>C) packet's bunches and update
    /// cs.last_outgoing_reliable_chseq with the highest chSeq seen per
    /// channel.  S>C bunch header layout (from send_client_ack_good_move
    /// and friends):
    ///   bControl(1) + bIsRepPaused(1) + bReliable(1) + ChIdx(SIP)
    ///   + bExports(1) + bGuids(1) + bPartial(1)
    ///   + ChSeq(12) IF bReliable
    ///   + BunchDataBits(13)
    ///   + payload bits
    /// This is best-effort: if parsing fails midway, we stop scanning.
    /// Called after every send_captured_packet so we know what chSeq the
    /// spliced bytes carried.  Used by send_client_ack_update_level_visibility
    /// to pick the next contiguous chSeq for native ch=3 sends.
    void scan_outgoing_packet_chseq(ClientState& cs,
                                     const uint8_t* buf, size_t pkt_len) {
        if (pkt_len < 8) return;
        // Strip termination + skip outer header + notify + custom field + pkt_info
        size_t eff_bits = ::ue5::strip_termination(buf, pkt_len);
        // Outer header (38 bits) + Notify (32 packed + N×32 history) + custom (48) + pkt_info (12)
        // Read packed_header first to know history count
        size_t off = 38;
        if (off + 32 > eff_bits) return;
        uint32_t packed = static_cast<uint32_t>(::ue5::read_bits(buf, pkt_len, off, 32));
        uint32_t hist_count = (packed & 0xF) + 1;
        if (off + hist_count * 32 > eff_bits) return;
        for (uint32_t i = 0; i < hist_count; ++i)
            ::ue5::read_bits(buf, pkt_len, off, 32);
        // AoC custom field (48 bits)
        if (off + ClientState::CUSTOM_FIELD_BITS > eff_bits) return;
        off += ClientState::CUSTOM_FIELD_BITS;
        // PacketInfo per AoC layout (matches build_replay_packet, lines 5158-5166):
        //   has_pkt_info (1)
        //   IF has_pkt_info: jitter (10)
        //   has_srv_frame (1)        ← UNCONDITIONAL after the optional jitter
        //   IF has_srv_frame: frame_time (8)
        // Earlier iteration of this scanner had has_srv_frame nested INSIDE
        // has_pkt_info which caused 10-bit misalignment for packets that
        // happened to have has_pkt_info=0 — symptom was ch=3 chSeq tracker
        // reading 1978 from misaligned bits in the 09:54 test.
        if (off + 1 > eff_bits) return;
        bool has_pkt_info = ::ue5::read_bits(buf, pkt_len, off, 1) != 0;
        if (has_pkt_info) {
            if (off + 10 > eff_bits) return;
            ::ue5::read_bits(buf, pkt_len, off, 10);  // jitter
        }
        if (off + 1 > eff_bits) return;
        bool has_srv = ::ue5::read_bits(buf, pkt_len, off, 1) != 0;
        if (has_srv) {
            if (off + 8 > eff_bits) return;
            ::ue5::read_bits(buf, pkt_len, off, 8);  // server frame time
        }
// Cyb with C.O.A

        // Walk bunches
        int bunch_idx = 0;
        while (off + 20 <= eff_bits && bunch_idx < 32) {
            ++bunch_idx;
            size_t bunch_start = off;
            bool b_ctrl = ::ue5::read_bits(buf, pkt_len, off, 1) != 0;
            // S>C bunches we generate don't write bOpen/bClose (we only ever
            // write bCtrl=0 for non-control), but spliced captured bunches
            // MIGHT have ctrl/open/close set.  Handle both.
            bool b_open = false, b_close = false;
            if (b_ctrl) {
                if (off + 2 > eff_bits) return;
                b_open  = ::ue5::read_bits(buf, pkt_len, off, 1) != 0;
                b_close = ::ue5::read_bits(buf, pkt_len, off, 1) != 0;
                if (b_close) {
                    // ── PM14 (2026-04-28) — variable-length CloseReason ──
                    // Per RE of sub_144230D50 line 330: AOC reads
                    //   (*a2->vtable[50])(a2, &v167, 15)
                    // i.e. SerializeInt(MAX=15) which is variable 1-4 bits
                    // (typically 4 bits for values 0-7, 3 bits for 7-14).
                    // Prior fixed 3-bit read mis-aligned by 1 bit when bClose=1
                    // appeared in spliced PC chain bunches → CNSF cascade.
                    if (off + 4 > eff_bits) return;
                    (void)::ue5::read_serialize_int(buf, pkt_len, off, 15);
                }
            }
            if (off + 2 > eff_bits) return;
            ::ue5::read_bits(buf, pkt_len, off, 1);  // bIsRepPaused
            bool b_reliable = ::ue5::read_bits(buf, pkt_len, off, 1) != 0;
            uint32_t ch_idx = static_cast<uint32_t>(::ue5::read_sip(buf, pkt_len, off));
            if (off + 3 > eff_bits) return;
            ::ue5::read_bits(buf, pkt_len, off, 1);  // bExports
            ::ue5::read_bits(buf, pkt_len, off, 1);  // bGuids
            bool b_partial = ::ue5::read_bits(buf, pkt_len, off, 1) != 0;
            uint16_t ch_seq = 0;
            if (b_reliable) {
                // ── Phase B.0p9 (2026-04-28 PM9) — ChSeq is 10-bit ──
                // AOC encodes ChSeq via serialize_int(value, 1024), which
                // reads/writes ceil(log2(1024)) = 10 bits.  Reading 12 bits
                // here previously consumed 2 extra bits that belonged to the
                // following ChName.bIsHardcoded + SIP fields, corrupting our
                // tracker and any downstream native send that read it.
                if (off + 10 > eff_bits) return;
                ch_seq = static_cast<uint16_t>(::ue5::read_bits(buf, pkt_len, off, 10));
                // Update tracker.
                //
                // ── Phase B.0p10 (2026-04-28 PM10) — first-touch fix ──
                // PM9's modulo-aware "later than" check used a half-window
                // (512) to avoid treating wrap-around as advancement.  But
                // that test rejected the FIRST seen value when cur==0 and
                // ch_seq>=512.  Captured PC chain bunches start at ch_seq=954
                // (10-bit), so cur stayed at 0 and our native ClientRestart
                // emitted at chSeq=1 instead of 958, landing far before the
                // client's InRel[3]=957 → bunch buffered indefinitely.
                //
                // Fix: treat cur==0 as uninitialized (default-init from
                // unordered_map operator[]).  Real chSeq values for ch>=3
                // start at the captured PC chain's chSeq=954 in our session,
                // so 0 is unambiguous as "not yet seen".  For ch=0 control
                // channel, we don't use this tracker (send_ch0_reliable_payload
                // uses cs.reliable_seq directly).
                auto& cur = cs.last_outgoing_reliable_chseq[ch_idx];
                if (cur == 0) {
                    cur = ch_seq;
                } else {
                    uint16_t diff = (ch_seq - cur) & 0x3FF;
                    if (diff > 0 && diff < 512) {
                        cur = ch_seq;
                    }
                }
            }
            if (off + 13 > eff_bits) return;
            uint32_t bunch_data_bits =
                static_cast<uint32_t>(::ue5::read_bits(buf, pkt_len, off, 13));
            // Skip payload — we don't care about bunch contents here
            if (off + bunch_data_bits > eff_bits) return;
            off += bunch_data_bits;
            // Sanity break — if a bunch advanced us 0 bits, abort
            if (off == bunch_start) return;
            // Suppress unused warnings
            (void)b_open; (void)b_partial;
        }
    }

    bool send_captured_packet(const std::string& client_key,
                                const sockaddr_in& addr,
                                uint32_t replay_idx) override {
        // Road A — Phase B.0.  Mirror the per-packet emission half of
        // replay_loop, but driven externally by WorldBootstrapEmitter
        // instead of the legacy replay thread.  This lets the native
        // sequencer act as the SOLE driver of the bootstrap stream
        // (no parallel replay thread), while still emitting captured
        // bytes for the ~75 packets we haven't yet promoted to native.
        if (!replay_data_ || replay_data_->packets.empty()) {
            spdlog::warn("[GameServer] send_captured_packet: no replay loaded");
            return false;
        }
        if (replay_idx >= replay_data_->packets.size()) {
            spdlog::warn("[GameServer] send_captured_packet: idx {} out of "
                          "range (have {} packets)",
                          replay_idx, replay_data_->packets.size());
            return false;
        }
        const ReplayPacketInfo& rpkt = replay_data_->packets[replay_idx];
        if (rpkt.bunch_bits == 0) {
            // Sentinel/keepalive — caller should have classified this
            // as Skip in the plan.  Quiet info, not warn.
            spdlog::info("[GameServer] send_captured_packet: idx {} is "
                          "sentinel (bunch_bits=0) — skipping",
                          replay_idx);
            return true;  // not an error, just nothing to send
        }

        // ── PM29 (2026-04-29) — REVERTED ──────────────────────────────
        // Tested skip of replay_idx=241 — CNSF still occurred 378ms later
        // at frame 9 with same Size=0x68000000.  Pkt241 NOT the trigger.
        // Skip removed.  See PM30 below for actual investigation.
// -- C.Y.B x CoA --

        // ── PM34 (2026-04-29) — REVERTED in PM35 ─────────────────────
        // PM34 had a single-pkt skip for replay_idx=46.  PM35 (Path B2
        // Step 1) replaces the entire captured PC chain (replay_idx
        // 22-46) with native emit via PcEmitter, so the per-pkt skip
        // is now redundant — the world_bootstrap_plan.cpp marks all
        // those entries as Skip mode and they never reach this fn.
        // Keeping the comment for traceability.

        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(client_key);
        if (it == clients_.end()) return false;
        if (it->second.phase < ClientState::CONNECTED) return false;
        ClientState& cs = it->second;

        // Reuse the existing builder.  Same code path the replay thread
        // uses, so we get identical wire output.
        uint8_t buf[2048] = {};
        size_t pkt_len = build_replay_packet(buf, sizeof(buf), rpkt, cs);
        if (pkt_len == 0) {
            spdlog::warn("[GameServer] send_captured_packet: build returned "
                          "0 for idx {} (size {} bunch_bits {})",
                          replay_idx, rpkt.raw.size(), rpkt.bunch_bits);
            return false;
        }

        // ── Phase B.0p4 (2026-04-27 morning) ──────────────────────────
        // NetGUID-88 collision detector (per UPackageMapClient::SerializeNewActor RE).
        //
        // The captured Pawn (pkt#78) uses NetGUID ObjectId=88 (BARE form
        // SIP(88)+0x2a flag).  The client's PackageMap caches a mapping
        // NetGUID → class.  If any earlier packet exports NetGUID 88 to
        // a different class, pkt#78's spawn fails silently (the
        // sub_144285F10 NOT_IN_CACHE / class-mismatch path).
        //
        // Scan every spliced packet's raw bytes for the SIP(88)=0xb0 byte
        // pattern and the inline u64 ObjectId=88 pattern.  Log every hit
        // so we can correlate with packet index.  One-time diagnostic;
        // remove once the question is answered.
        if (rpkt.bunch_bits > 0 && replay_idx <= 78) {
            const uint8_t* d = rpkt.raw.data();
            const size_t   n = rpkt.raw.size();
            int sip_hits = 0;
            int inline_hits = 0;
            // SIP-encoded BARE NetGUID 88: byte = (88<<1)|0 = 0xb0
            // Followed typically by the 0x2a flag byte.
            for (size_t i = 0; i + 1 < n; ++i) {
                if (d[i] == 0xb0 && d[i+1] == 0x2a) ++sip_hits;
            }
            // Inline 16-byte FIntrepidNetGUID with ObjectId=88 LE
            // (the 8 ObjectId bytes are 58 00 00 00 00 00 00 00, then any
            // ServerId+Randomizer follows).  Per sub_1413A6340 RE: hash key
            // is the full 16 bytes, so different ServerId/Randomizer values
            // would hash differently — useful to detect even non-overlapping
            // ObjectId=88 exports that could still cause an ObjectId→FullGUID
            // table conflict.
            for (size_t i = 0; i + 15 < n; ++i) {
                if (d[i]==0x58 && d[i+1]==0 && d[i+2]==0 && d[i+3]==0 &&
                    d[i+4]==0 && d[i+5]==0 && d[i+6]==0 && d[i+7]==0) {
                    ++inline_hits;
                    spdlog::warn("[GUID88-SCAN]   inline at byte {} : "
                                  "ServerId={} Randomizer={}",
                                  i,
                                  *reinterpret_cast<const uint32_t*>(&d[i+8]),
                                  *reinterpret_cast<const uint32_t*>(&d[i+12]));
                }
            }
            if (sip_hits || inline_hits) {
                spdlog::warn("[GUID88-SCAN] replay_idx={} bunch_bits={}: "
                              "SIP(0xb0+0x2a)={} | inline_88_xxxx={}",
                              replay_idx, rpkt.bunch_bits, sip_hits, inline_hits);
            }
        }

        const uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        // Phase B.0e2 (2026-04-27): scan the OUTGOING packet's bunches and
        // update cs.last_outgoing_reliable_chseq so a later native send on
        // the same channel (e.g., the SULV ack on ch=3) can use the correct
        // next chSeq.
        scan_outgoing_packet_chseq(cs, buf, pkt_len);

        int sent = send_to_client_impl(buf, pkt_len, addr);
        if (sent > 0) {
            spdlog::info("[GameServer] >> [SPLICE] replay_idx={} ({}B, "
                          "{} bunch bits) seq={}",
                          replay_idx, pkt_len, rpkt.bunch_bits, sent_seq);
            return true;
        }
        spdlog::warn("[GameServer] send_captured_packet: send_to_client failed "
                      "for idx {}", replay_idx);
        return false;
    }

    uint32_t loaded_replay_packet_count() const override {
        if (!replay_data_) return 0;
        return static_cast<uint32_t>(replay_data_->packets.size());
    }

    const std::vector<uint8_t>* captured_packet_raw(uint32_t replay_idx) const override {
        if (!replay_data_ || replay_idx >= replay_data_->packets.size()) return nullptr;
        return &replay_data_->packets[replay_idx].raw;
    }

    // ───────────────────────────────────────────────────────────────────
    //  on_world_bootstrap_complete — Phase B.0e2 (2026-04-27)
    //
    //  Called by WorldBootstrapEmitter::emit_all once it finishes walking
    //  the plan.  We drain cs.pending_sulv_acks here, sending each queued
    //  ACK with a chSeq taken from cs.last_outgoing_reliable_chseq[3] + 1
    //  (which the post-build scanner has tracked across every spliced
    //  ch=3 reliable bunch).
    //
    //  At this point cs.last_outgoing_reliable_chseq[3] reflects the
    //  HIGHEST chSeq we shipped on ch=3 during splice.  Each ACK we send
    //  bumps it by 1 and the next ACK uses the new value — so successive
    //  ACKs are contiguous without gaps.
    // ───────────────────────────────────────────────────────────────────
    void on_world_bootstrap_complete(const std::string& client_key,
                                       const sockaddr_in& addr) override {
        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(client_key);
        if (it == clients_.end()) return;
        ClientState& cs = it->second;
        cs.world_bootstrap_complete = true;

        // The Phase B.0p fuzz that used to fire here (six send_client_restart_native
        // variants, field index encoded wrongly) is gone.  PcEmitter already sends
        // ClientRestart (handle 45, AoC field-loop parameters, PM95) inside the PC
        // bunch; that is the variant the May sessions report possession with
        // (PM107).  --answer-possession-checks adds a stock-format ClientRestart
        // (protocol/wire/possession.h) here for comparison.
        if (config_.answer_possession_checks && cs.pkt78_emitted && !cs.reactive_clientrestart_sent) {
            cs.reactive_clientrestart_sent = true;
            spdlog::warn("[POST-BOOTSTRAP] sending ClientRestart for the spliced pawn");
            (void)send_client_restart(cs, addr, client_key);
        }
// built with CoA and C | Y | B

        if (cs.pending_sulv_acks.empty()) {
            spdlog::info("[GameServer] on_world_bootstrap_complete: no SULV "
                          "acks queued — nothing to drain");
            return;
        }

        uint16_t ch3_seq_seen = 0;
        auto trk = cs.last_outgoing_reliable_chseq.find(3);
        if (trk != cs.last_outgoing_reliable_chseq.end()) {
            ch3_seq_seen = trk->second;
        }
        spdlog::warn("[GameServer] Draining {} pending SULV ACKs (ch=3 last "
                      "outgoing chSeq={})",
                      cs.pending_sulv_acks.size(), ch3_seq_seen);

        // Move queue out so we can release the lock during sends (the send
        // function takes the lock internally via cs reference; we already
        // hold it, so we'll send under the same lock — that's OK).
        auto queue = std::move(cs.pending_sulv_acks);
        cs.pending_sulv_acks.clear();

        for (const auto& ack : queue) {
            // send_client_ack_update_level_visibility reads
            // cs.last_outgoing_reliable_chseq[3] and bumps it for the next
            // call, so successive ACKs pick contiguous chSeq values.
            (void)send_client_ack_update_level_visibility(
                cs, addr, client_key, ack.request, ack.triggering_ch_idx);
        }
    }

    // ───────────────────────────────────────────────────────────────────
    //  send_client_initialize_character_native — REWRITTEN 2026-05-05
    //
    //  WAS (PM28, 2026-04-29): wrong signature `ClientInitializeCharacter(FName)`,
    //  wrong handle encoding (SIP instead of SerializeInt), small-range fuzz.
    //
    //  NOW: SDK-verified signature `void ClientInitializeCharacter()` —
    //  zero parameters (per `GameSystemsPlugin_classes.hpp:15007`).
    //
    //  Wire handle from `docs/RE-CLIENTINITIALIZECHARACTER-HANDLE.md`:
    //    wire_handle = 142  (DERIVED-FROM-RE, ±10)
    //    Probable range: 138..150
    //    Primary fuzz order: 142 → 141/143 → 138/144/146 → 128/129
    //
    //  Wire format per PM95 (proven via ClientRestart's bare_mode=4):
    //    SerializeInt(handle, MAX=1024)  10 bits  (PM97 verified)
    //    1-bit advance                    1 bit
    //    SIP(0) terminator                8 bits  (= byte 0x00, no fields)
    //  Total inner = 19 bits.
    //
    //  Probe knobs (no rebuild required to iterate):
    //    probe_cic_handle.txt    — wire handle (default 142)
    //    probe_cic_max.txt       — SerializeInt MAX (default 1024 = 10 bits)
    //
    //  Called when we recognize incoming ServerRequestInitializeCharacter
    //  (PM28's recognizer at line 4471 still works — character_name is now
    //  ignored, kept for API stability).
    // ───────────────────────────────────────────────────────────────────
    bool send_client_initialize_character_native(
            const std::string& client_key,
            const sockaddr_in& addr,
            const std::string& character_name) {
        (void)character_name;  // SDK confirms ClientInitializeCharacter() takes NO params

        // 2026-05-06 — REWRITTEN to use PropertyUpdateBunchBuilder, which is
        // the SAME path that ClientRestart RPC uses successfully (proven by
        // PM97 — possession works).  The PRIOR manual bunch construction
        // produced a "Bunch data overflowed" error on the client (per
        // AOC.log).
        //
        // Probe knobs (no rebuild required):
        //   probe_cic_handle.txt   — wire handle (default 142, per Track-1 RE ±10)
        //   probe_cic_max.txt      — SerializeInt MAX (default 1024)
        //   probe_cic_chseq.txt    — ch=3 reliable chSeq (default 957)
        //   probe_cic_autofuzz.txt — if "1", auto-increment handle each restart
        //                            using probe_cic_handle.txt as state
        auto read_probe_uint = [](const char* path, uint32_t default_val) -> uint32_t {
            std::FILE* fp = std::fopen(path, "r");
            if (!fp) return default_val;
            uint32_t n = default_val;
            std::fscanf(fp, "%u", &n);
            std::fclose(fp);
            return n;
        };
        auto write_probe_uint = [](const char* path, uint32_t value) {
            std::FILE* fp = std::fopen(path, "w");
            if (!fp) return;
            std::fprintf(fp, "%u\n", value);
            std::fclose(fp);
        };
        const uint32_t wire_handle = read_probe_uint("probe_cic_handle.txt", 142u);
        const uint32_t handle_max  = read_probe_uint("probe_cic_max.txt",   1024u);
        const uint32_t cic_chseq   = read_probe_uint("probe_cic_chseq.txt",  957u);
        const uint32_t auto_fuzz   = read_probe_uint("probe_cic_autofuzz.txt", 0u);

        // Auto-fuzz: write the NEXT handle to try after this run.
        // 2026-05-06 update: empirical findings via dispatch:
        //   handle 142 → ClientDebugNpc (SDK pos 12)
        //   handle 160 → ClientGMReceiveServiceBuildingPlotData (not in SDK,
        //                added in current build between SDK pos 25 and 26)
        // Target ClientInitializeCharacter alphabetizes between
        // ClientHitConfirmationsReliable (SDK pos 29) and ClientInterruptCraft
        // (pos 31).  Walking upward from 160 to find it.
        if (auto_fuzz != 0) {
            static const uint32_t fuzz_order[] = {
                160, 161, 162, 163, 164, 165, 166, 167, 168, 169,
                170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180
            };
            const size_t fuzz_count = sizeof(fuzz_order) / sizeof(fuzz_order[0]);
            uint32_t next = fuzz_order[0];
            for (size_t i = 0; i < fuzz_count; ++i) {
                if (fuzz_order[i] == wire_handle) {
                    next = fuzz_order[(i + 1) % fuzz_count];
                    break;
                }
            }
            write_probe_uint("probe_cic_handle.txt", next);
            spdlog::warn("[CIC-FUZZ] this run handle={} → next launch will try handle={}",
                         wire_handle, next);
        }

        spdlog::warn("[CIC] ClientInitializeCharacter() — wire_handle={} "
                     "(MAX={})  [SDK: zero params, builder-routed]",
                     wire_handle, handle_max);
// C_Y_B + C.O.A

        // ── Build via PropertyUpdateBunchBuilder (mirrors emit_pawn_link) ──
        aoc::protocol::emit::PropertyUpdateBunchBuilder b;
        b.set_channel(3);
        b.set_ch_sequence(static_cast<uint16_t>(cic_chseq & 0x3FFu));
        b.set_reliable(true);
        b.set_ch_name_hardcoded(102);   // NAME_Actor (matches PC channel open)
        b.set_use_modern_inner_format(true);  // proven via ClientRestart

        b.v3_begin_content_block_channel_actor(handle_max);
        b.v3_add_rpc_no_params(wire_handle);
        b.v3_end_content_block();
        b.v3_finish_bunch();

        aoc::protocol::emit::BunchWriter bw;
        const size_t bunch_bits = b.build(bw);
        if (bunch_bits == 0) {
            spdlog::error("[CIC] builder returned 0 bits");
            return false;
        }

        const bool ok = send_bunch_packet(client_key, addr, bw.data(), bunch_bits);
        spdlog::warn("[GameServer] >> ★ NATIVE ClientInitializeCharacter() "
                      "(NO PARAMS) wire_handle={} chSeq={} bunch_bits={} ok={}",
                      wire_handle, cic_chseq, bunch_bits, ok);
        return ok;
    }

    // ───────────────────────────────────────────────────────────────────
    //  send_client_restart_native — Phase B.0p (2026-04-27)
    //
    //  CLEAN REWRITE using captured RE evidence.  Replaces the 20-variant
    //  fuzz that overrode the captured ClientRestart with a synthetic
    //  128-bit NetGUID nothing on the client side recognized.
    //
    //  Pawn parameter encoding (decoded from captured_pkt_78.bin bunch 2):
    //    BARE format: SIP(ObjectId) + 0x2a flag byte
    //    For ObjectId 88 (the captured Pawn): 1 byte SIP + 1 byte flag = 16 bits
    //    NOT the 128-bit FIntrepidNetGUID inline form we'd been emitting.
    //
    //  When called from the NMT-DETECT reactive path, pass:
    //    obj_id=88, server_id=ignored, randomizer=ignored
    //  to reference the captured Pawn that pkt#78's splice already
    //  registered in the client's PackageMap.
    //
    //  Bunch envelope (per sub_143F2A2A0/sub_143F2DA40 chain):
    //    bControl=0, bIsRepPaused=0, bReliable=1, ChIdx=3 (PC channel),
    //    bHasPME=0, bHasMBG=0, bPartial=0,
    //    ChSeq (12-bit), ChName.bIsHardcoded=1 + EName=102 (NAME_Actor),
    //    BunchDataBits (13-bit)
    //
    //  Content block + outer payload (per sub_1444E5420 + sub_1444E55B0):
    //    [bHasRepLayout=0][bIsActor=1][SIP outer_payload_bits]
    //    [outer payload {
    //       [1 bit] header (skipped by sub_1444E5420 lines 21-28)
    //       [SIP] handle+1 (0-based handle, 0 reserved as terminator)
    //       [SIP] field_payload_bits = 16 (BARE NetGUID size)
    //       [SIP(obj_id) + 0x2a] BARE Pawn NetGUID
    //       [SIP] 0  ← REQUIRED end-of-fields terminator
    //    }]
    //  + extra '1' sentinel bit + add_termination (AoC PacketHandler
    //    requirement, see send_keepalive ~5659).
    bool send_client_restart_native(const std::string& client_key,
                                      const sockaddr_in& addr,
                                      uint64_t pawn_obj_id,
                                      uint32_t pawn_server_id,
                                      uint32_t pawn_randomizer) override {
        // PM23 (2026-04-28): use FULL FIntrepidNetGUID (128 bits), not BARE.
        // (Prior `(void)` annotations removed - we now use these fields.)

        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(client_key);
        if (it == clients_.end()) return false;
        if (it->second.phase < ClientState::CONNECTED) return false;
        ClientState& cs = it->second;

        // ── Phase B.0p11 (2026-04-28 PM11) — chSeq with safe floor past PC chain ──
        //
        // Previous attempts:
        //   - PM7: hardcoded 2080 — gap from real session
        //   - PM8: cs.last_outgoing_reliable_chseq[3]+1 — but tracker reads
        //          12 bits, conflated chSeq with neighboring fields
        //   - PM9: tracker reads 10 bits — correct, but also exposes that
        //          scan_outgoing_packet_chseq only walks bunch[0] of each
        //          packet (multi-bunch partial chains don't advance tracker)
        //   - PM10: cur==0 first-touch fix — tracker advances on each spliced
        //          packet's bunch[0], but stops at 955 because subsequent
        //          bunches in pkt#22 (chSeq 956, 957) aren't tracked
        //
        // Live test PM10: ClientRestart fired at chSeq=955+1=956, conflicting
        // with captured pkt#22 bunch[2] at chSeq=956 (same chSeq).  Client
        // dropped one as duplicate.  Loading screen alternates between
        // "Waiting for WP Streaming" (60s) and "No valid pawn" (recreated)
        // — flashing 0-100% pattern.
        //
        // Fix (PM11): apply a SAFETY FLOOR.  Captured PC chain occupies
        // chSeqs 954..957 (10-bit).  Continuation bunches in pkts 47-149
        // span up to ~970-980.  Use floor of 1000 to clear all of them.
        // Higher chSeqs in pkts 150-500 (deep splice) might wrap past 1023
        // back into single digits, but the client's reliable-seq window
        // tolerates wrap-back as forward via the half-window rule.
        // ── PM26 (2026-04-29) — REMOVE chSeq floor, use exact tracker+1 ──
        //
        // PM11's `kMinChSeqForCR=1000` floor was assumed to "clear" the PC
        // chain.  But UE5 reliable channels are STRICTLY contiguous.  If
        // client's InReliable[3] is at 985 and we send chSeq=1000, the
        // client QUEUES our bunch waiting for chSeq 986-999 that never
        // arrive — bunch sits in queue forever, never executed.
        //
        // This explains why ALL native CR attempts in PM7-PM25 silently
        // failed (queued, not processed).  The fix: use exact tracker+1,
        // NO floor.
        //
        // The tracker advances correctly for every bunch we ship (after
        // the PM26 multi-bunch tracker fix in scan_outgoing_packet_chseq).
        uint16_t last_ch3_seen = 0;
        auto trk = cs.last_outgoing_reliable_chseq.find(3);
        if (trk != cs.last_outgoing_reliable_chseq.end()) {
            last_ch3_seen = trk->second;
        }
        // Use exact next-in-sequence chSeq.  Wrap at 1024 (10-bit).
        uint16_t candidate = static_cast<uint16_t>((last_ch3_seen + 1u) & 0x3FFu);
        if (candidate == 0) candidate = 1;  // never use 0 (reserved for unreliable)
        const uint32_t this_chseq_value = candidate;
        // Bump tracker so subsequent ch=3 sends pick the next contiguous
        // chSeq.  The send_bunch_packet path (used by SULV ack) does the
        // same bookkeeping after its scan_outgoing_packet_chseq pass; we do
        // it inline here because we're not going through that wrapper.
        cs.last_outgoing_reliable_chseq[3] = static_cast<uint16_t>(this_chseq_value);
// (C.Y.B : C-O-A)

        // ── Phase B.0p12 (2026-04-28 PM12) — handle FUZZ across sessions ──
        //
        // PM7-PM11: chSeq alignment fixes succeeded.  Bunch now reaches the
        // client.  But handle=52 doesn't elicit any C>S Server* RPC response,
        // meaning either it's out-of-bounds or it's a property (not RPC).
        //
        // PM12 strategy: rotate through candidate handles, one per session.
        // Counter persists across reconnects (static thread_local).  After
        // each test, user reconnects → next handle is tried.  Watch C>S for
        // dispatch byte 0x76 (ServerAcknowledgePossession = SUCCESS) or
        // 0x7E (ServerCheckClientPossession = handle right, Pawn ref wrong).
        //
        // Candidate list (priority order):
        //   25, 26 — alphabetical pos in AOC Client* table (0/1 indexed)
        //   27-32 — adjacent (AOC's 18 added Client* RPCs may shift handle)
        //   50, 51, 53, 54, 55, 56 — if Server*=N preceding, then offset
        //   22 — same as ServerNotifyLoadedWorld empirically
        //   60, 65, 70 — far-out speculation
        static constexpr uint16_t kHandleFuzzList[] = {
            25, 26, 27, 28, 29, 30, 31, 32, 50, 51, 53, 54, 55, 56, 22, 60, 65, 70
        };
        static constexpr size_t kHandleFuzzCount =
            sizeof(kHandleFuzzList) / sizeof(kHandleFuzzList[0]);
        static thread_local size_t fuzz_idx = 0;
        const uint16_t kFieldHandle = kHandleFuzzList[fuzz_idx % kHandleFuzzCount];
        spdlog::warn("[FUZZ] ClientRestart attempt with handle={} "
                      "(fuzz {} of {})",
                      kFieldHandle,
                      (fuzz_idx % kHandleFuzzCount) + 1,
                      kHandleFuzzCount);
        ++fuzz_idx;
        // ── PM23 (2026-04-28) — wire_handle uses CALV +5 reserved offset ──
        //
        // PM22 retest with wire_handle = kFieldHandle + 1 (for handle=25,
        // wire_handle=26) elicited NO ServerAcknowledgePossession from client.
        // The bunch was clean (no CNSF) but possession didn't trigger.
        //
        // Comparison with CALV (which works after PM21 fix):
        //   CALV (alphabetical pos 2 1-indexed): wire_idx = 2 + 5 = 7
        //   Dispatch byte = (7<<1)|0 = 0x0E
        //
        // ClientRestart (alphabetical pos 26 1-indexed = 25 0-indexed):
        //   wire_idx = 26 + 5 = 31
        //   Our kFieldHandle is 0-indexed alphabetical pos.
        //   Therefore wire_handle = kFieldHandle + 6 (= +1 to make 1-indexed +5 reserved)
        //
        // For the default fuzz handle 25: wire_handle = 25 + 6 = 31 (matches PM7 prediction)
        const uint32_t wire_handle = static_cast<uint32_t>(kFieldHandle) + 6u;

        // ── PM24 (2026-04-29) — Pawn NetGUID candidates from packet scan ──
        //
        // PM18-PM23 RE confirmed AOC NetGUID is 128 bits (sub_14141E960
        // reads 4× uint32 raw, sub_14426F930 cache hash uses 16 bytes).
        // Layout: {ObjectId u64, ServerId u32, Randomizer u32}.
        //
        // scan_actor_opens_pm24.py extracted NetGUIDs from all 1377 actor
        // opens in replay_data.bin.  The PC is at pkt 22 ch=3 with:
        //   ObjectId u64 = 0x2254786600000006
        //   ServerId u32 = 0x613FAFEB
        //   Randomizer u32 = 0
        //
        // The Pawn's NetGUID is one of the SUBSEQUENT actor opens.  Strong
        // candidate: pkt 62 ch=14 (40 pkts after PC, biggest BDB=7329 bits).
        //
        // Cycle through candidates via thread_local fuzz_idx; each test
        // session = one attempt.  Server-restart advances to next candidate.
        struct PawnGuidCandidate {
            const char* label;
            uint64_t object_id;
            uint32_t server_id;
            uint32_t randomizer;
        };
        static constexpr PawnGuidCandidate kPawnCandidates[] = {
            // Top candidate first (large BDB suggests Pawn-class actor)
            {"pkt62 ch=14",       0xF47BBB2E0000000Eull, 0xDAB716DCu, 0u},
            {"pkt120 ch=24",      0xDD17764200000012ull, 0x39690C62u, 0u},
            {"pkt127 ch=28",      0x4005739A00000010ull, 0x87269152u, 0u},
            {"pkt136 ch=32",      0xECAC96D604000000ull, 0x28ADD85Au, 0u},
            {"pkt144 ch=36",      0x4EC4561E00000010ull, 0x8BC3F1CEu, 0u},
            // Reference: PC's own NetGUID (not Pawn, but for sanity check)
            {"PC pkt22 ch=3",     0x2254786600000006ull, 0x613FAFEBu, 0u},
        };
        static constexpr size_t kPawnCandidateCount =
            sizeof(kPawnCandidates) / sizeof(kPawnCandidates[0]);
        static thread_local size_t pawn_idx = 0;
        const auto& pawn_cand = kPawnCandidates[pawn_idx % kPawnCandidateCount];
        // Override caller-provided values with our candidate
        pawn_obj_id = pawn_cand.object_id;
        pawn_server_id = pawn_cand.server_id;
        pawn_randomizer = pawn_cand.randomizer;
        spdlog::warn("[PAWN-FUZZ] Trying Pawn candidate '{}' "
                     "(obj=0x{:016x} srv=0x{:08x} rnd=0x{:08x})",
                     pawn_cand.label, pawn_obj_id, pawn_server_id, pawn_randomizer);
        ++pawn_idx;
// ~ C | Y | B & CoA ~

        auto sip_bit_count = [](uint32_t v) -> size_t {
            if (v == 0) return 8;
            size_t bits = 0;
            while (v > 0) { bits += 8; v >>= 7; }
            return bits;
        };

        // ── PM23 (2026-04-28) — FULL FIntrepidNetGUID (128 bits) ──
        //
        // PM22 retest with BARE NetGUID (16 bits = SIP+flag byte) elicited NO
        // possession ack. Per PM15 RE of sub_14141E960 (AOC NetGUID reader),
        // the wire format is 4× uint32 raw = 128 bits / 16 bytes:
        //   { ObjectId u64 @0..7,
        //     ServerId u32 @8..11,
        //     Randomizer u32 @12..15 }
        //
        // For NetGUID 88: bytes = 58 00 00 00 00 00 00 00 | 00 00 00 00 | 00 00 00 00.
        // The BARE encoding was a guess from PM7 that never validated.
        constexpr size_t pawn_netguid_bits = 128;

        const size_t handle_sip_bits     = sip_bit_count(wire_handle);
        const size_t size_sip_bits       = sip_bit_count(static_cast<uint32_t>(pawn_netguid_bits));
        const size_t terminator_sip_bits = 8;

        const size_t outer_payload_bits = 1u                        // header
                                        + handle_sip_bits
                                        + size_sip_bits
                                        + pawn_netguid_bits
                                        + terminator_sip_bits;
        const size_t outer_size_varint_bits =
            sip_bit_count(static_cast<uint32_t>(outer_payload_bits));
        const size_t total_bdb = 2 + outer_size_varint_bits + outer_payload_bits;

        // ── Bunch envelope ───────────────────────────────────────────────
        uint8_t bunch_buf[256] = {};
        size_t bb = 0;
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bControl=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bIsRepPaused=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 1, 1); // bReliable=1
        ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb, 3);    // ChIdx=3 (PC)
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bHasPME=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bHasMBG=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bPartial=0

        // ── Phase B.0p9 (2026-04-28 PM9) — ChSeq is 10-bit, not 12-bit ──
        //
        // Live-test PM8 fired ClientRestart at chSeq=1979 (from session
        // tracker), but the client closed the connection with:
        //   "Existing channel at index 3 with type \"Actor\" differs from
        //    the incoming bunch's expected channel type, \"UInt32Property\""
        //   Result=BunchWrongChannelType
        //
        // Root cause: ChSeq is encoded with serialize_int(value, 1024),
        // which reads ceil(log2(1024)) = 10 bits — NOT 12 bits as previously
        // written.  Captured PC chain pkt#22 bunch[0] hdr_bits=51 confirmed:
        //   1 ctrl + 1 open + 1 close + 1 paused + 1 reliable
        //   + 8 chidx + 1 exp + 1 mbg + 1 partial
        //   + 10 chseq                                             ← 10-bit!
        //   + 3 partial fields + 1 hardcoded + 8 SIP(102) + 13 bdb
        //   = 51 bits  ✓
        //
        // Writing 12 bits caused a 2-bit overshoot.  The client read our
        // chSeq as 10 bits (truncating) and then read the next 2 bits as
        // ChName.bIsHardcoded + SIP-byte continue, which decoded to a
        // garbage FName index that mapped to "UInt32Property" → channel
        // type mismatch → connection closed.
        const uint32_t this_chseq = this_chseq_value;
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                        this_chseq & 0x3FF, 10);                 // ChSeq 10-bit
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 1, 1); // ChName.bIsHardcoded=1
        ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb, 102);  // EName=102 NAME_Actor
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                        total_bdb, 13);                          // BunchDataBits

        // ── Content block envelope ──
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bHasRepLayout=0
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 1, 1); // bIsActor=1
        ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb,
                        static_cast<uint32_t>(outer_payload_bits));

        // ── Outer payload ──
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // header
        ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb, wire_handle);
        ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb,
                        static_cast<uint32_t>(pawn_netguid_bits));

        // ── PM23 (2026-04-28) — FULL FIntrepidNetGUID (128 bits) ──
        // Pawn parameter — write 4× uint32 raw matching sub_14141E960.
        //
        // Layout (each 32-bit field LE in stream order):
        //   bytes 0-3:  ObjectId low 32 bits
        //   bytes 4-7:  ObjectId high 32 bits
        //   bytes 8-11: ServerId
        //   bytes 12-15: Randomizer
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                        static_cast<uint32_t>(pawn_obj_id & 0xFFFFFFFFu), 32);  // ObjectId LSB
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                        static_cast<uint32_t>((pawn_obj_id >> 32) & 0xFFFFFFFFu), 32); // ObjectId MSB
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, pawn_server_id, 32); // ServerId
        ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, pawn_randomizer, 32); // Randomizer

        // End-of-fields terminator
        ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb, 0);

        const size_t bunch_bits = bb;

        // ── Wrap in UDP packet ──
        uint8_t buf[1024] = {};
        size_t off = 0;
        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);   // hasPktInfo
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10);  // jitter
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);   // hasFrameTime
// C:Y:B : C-O-A

        for (size_t i = 0; i < bunch_bits; ++i) {
            int bit = (bunch_buf[i >> 3] >> (i & 7)) & 1;
            ue5::write_bits(buf, sizeof(buf), off, bit, 1);
        }

        // AoC PacketHandler sentinel + standard termination
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);

        const uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        int sent = send_to_client_impl(buf, pkt_len, addr);
        if (sent > 0) {
            spdlog::warn("[GameServer] >> ★ NATIVE ClientRestart "
                          "(FULL FIntrepidNetGUID obj={} srv={} rnd={} "
                          "kFieldHandle={} wire_handle={} chSeq={} "
                          "bunch_bits={} pkt={}B seq={})",
                          pawn_obj_id, pawn_server_id, pawn_randomizer,
                          kFieldHandle, wire_handle, this_chseq,
                          bunch_bits, pkt_len, sent_seq);
            // Phase B.0p4 — flag the idempotent sentinel so the SNLW
            // handler won't fire ANOTHER ClientRestart on the next SNLW
            // retry.  This is crucial because PawnEmitter calls this
            // function directly (right after pkt#78 ships) AND the SNLW
            // detector may also call it later.  Whichever path fires
            // first, the second one short-circuits via this flag.
            cs.reactive_clientrestart_sent = true;
            return true;
        }
        spdlog::warn("[GameServer] send_client_restart_native: send failed");
        return false;
    }

    // ── REMOVED 2026-04-28 PM ──────────────────────────────────────────
    // `send_client_restart_native_FUZZ_LEGACY` (~250 lines) was dead code:
    //   - 20 hardcoded fuzz variants spanning 12 candidate handle values,
    //     2 header-bit options
    //   - Never called anywhere in the codebase (single grep hit was the
    //     definition itself)
    //   - Used a 128-bit FIntrepidNetGUID inline form for the Pawn
    //     parameter that turned out to be wrong — the captured pkt#78
    //     decode showed BARE form (SIP(ObjectId)+0x2a flag) is correct
    //   - Phase B.0p (2026-04-27) added the clean version above
    //     (`send_client_restart_native`) that uses the BARE form;
    //     this fuzzer was kept "for reference" but now obsoleted.
    //
    // If we need to fuzz handle values again in the future, do it inside
    // the clean function with a small loop, not by duplicating 250 lines.
    [[deprecated("removed 2026-04-28 — use send_client_restart_native")]]
    bool send_client_restart_native_FUZZ_LEGACY_REMOVED(const std::string&,
                                      const sockaddr_in&,
                                      uint64_t, uint32_t, uint32_t) {
        return false;
    }

#if 0  // ── HISTORIC FUZZ BODY (NEVER COMPILED) — kept for documentation only ──
        // Road A — Phase B.0j (2026-04-26 — FORMAT CORRECTED).
        //
        // After RE'ing UActorChannel::ProcessBunch (sub_143F2A2A0),
        // ReadContentBlockPayload (sub_143F2DA40), and
        // ReadContentBlockHeader (sub_143F2C340), the EXACT bunch
        // payload format is:
        //
        //   [bunch header — already done by caller]
        //   ── content block ──
        //   [1 bit]  bHasRepLayout = 0   (RPCs don't have RepLayout exports)
        //   [1 bit]  bIsActor = 1        (target = channel actor, no NetGUID)
        //   [SIP]    payload_bits = N    (size of inner payload)
        //   [N bits] payload {
        //       [SIP varint] function_index
        //       [params]     per UFunction.Children
        //   }
        //
        // Validated against captured ServerNotifyLoadedWorld bunch:
        // bytes 86 31 70... decode to bHasRepLayout=0, bIsActor=1,
        // payload_bits=816, 834 total bdb (2+16+816). ✓ MATCHES.
        //
        // Previous fuzz emits omitted the envelope, so 0x3E byte got
        // interpreted as bHasRepLayout=0, bIsActor=1, then varint of
        // garbage value → client silently dropped.
        //
        // Single-shot now (no fuzz) — function index 31 hypothesis
        // applies; if wrong we'll fuzz with the CORRECT envelope.
        // Phase B.0m (2026-04-26 night) — full inner-payload format.
        // After RE'ing sub_143F2F820 (ReceivedBunch), sub_143F2DC60
        // (ReadFieldHeaderAndPayload), and sub_1444E4A40 (RepLayout
        // exports — NOT called when bHasRepLayout=0), confirmed:
        //   - For RPC bunches (bHasRepLayout=0), field loop starts
        //     directly at bit 18 of bunch payload
        //   - field_handle is FIXED-WIDTH via SerializeInt(value, max)
        //     — width = ceil(log2(max+1)), where max = group->Count
        //
        // Phase B.0o (2026-04-26 latest) — *** TOTAL FORMAT REWRITE ***.
        //
        // After RE'ing sub_1444E5420 (Mode A entry) and the GOLDEN
        // sub_1444E55B0 (the actual field iteration loop), the wire
        // format is COMPLETELY DIFFERENT from anything we've tried:
        //
        //   ── inside the outer payload (after envelope+SIP size) ──
        //   [1 bit]  header   ← skipped/discarded by sub_1444E5420 lines 21-28
        //   ── per-field repeating block ──
        //   [SIP]    handle_plus_one    ← 0 = end-of-fields TERMINATOR!
        //                                   handle = wire_value - 1
        //   [SIP]    payload_bits       ← size of next field's payload
        //   [N bits] field_payload      ← e.g., 128 bits for FIntrepidNetGUID
        //   ── repeat fields ──
        //   [SIP]    0                  ← required terminator (1 byte 0x00)
        //
        // CRITICAL DECODING from sub_1444E55B0:
        //   line 179: (*virt+408)(v13, &v90)   ← READ HANDLE via SIP
        //   line 182: if (!v90) { v85=1; SUCCESS }  ← 0 = end
        //   line 187: v24 = v90 - 1            ← handle is 1-BASED on wire!
        //   line 189: bound check: v24 < group.Num()
        //   line 225: (*virt+408)(v115, &v98)  ← READ payload_bits via SIP
        //   line 248: sub_1414F3CC0(v107, v13, v98, 0) ← copy v98 bits
        //
        // We were sending fixed-width 9-bit handle. BOTH:
        //   1. Wrong encoding (should be SIP, not fixed-width)
        //   2. Missing the +1 offset (handle 0 = terminator!)
        //   3. Missing the SIP(0) terminator at end
        //   4. Maybe missing the 1-bit header at start
        //
        // No wonder the client silently drops every variant.
        //
        // For ClientRestart: handle is 0-based index into PC's group.
        // Most likely values: 4..30 (RPCs typically near group start).
        // Fuzz over reasonable handle values WITH and WITHOUT the
        // 1-bit header to nail both axes.
        struct FuzzVariant { bool has_header_bit; uint16_t handle; };
        static constexpr FuzzVariant kFuzzVariants[] = {
            // With 1-bit header (per sub_1444E5420 line 21-28 skip)
            {true,  12}, {true,  26}, {true,  56}, {true,  92},
            {true,   4}, {true,   8}, {true,  16}, {true,  30},
            {true,  62}, {true,   2}, {true,   1}, {true,   0},
            // Without 1-bit header (in case we're wrong about it)
            {false, 12}, {false, 26}, {false, 56}, {false, 92},
            {false,  4}, {false,  8}, {false, 16}, {false, 30},
        };
        constexpr size_t kFuzzCount = sizeof(kFuzzVariants)/sizeof(kFuzzVariants[0]);

        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(client_key);
        if (it == clients_.end()) return false;
        if (it->second.phase < ClientState::CONNECTED) return false;
        ClientState& cs = it->second;
// [C O A x Cyb]

        // Per-channel chSeq for ch=3.
        //
        // Phase B.0k (2026-04-26) — chSeq gap fix.  Previous chSeq=3000
        // left a ~1015-bunch gap on the reliable channel (spliced PC
        // chain ends around chSeq=1985).  UE5 reliable bunch reassembly
        // (per sub_143F32E00 IDA decomp) treats missing bunches as
        // pending retransmits — a 1015-bunch gap blocks ALL future
        // bunches and may trigger the client's connection-recycle
        // heuristic, causing the observed world-flash-then-reload cycle.
        //
        // Captured pkts 22-44 (PC chain) span ch=3 reliable chSeq 1978-1985.
        // Continuation bunches in pkts 47-149 may push ch=3 chSeq up to
        // ~2050.  Use chSeq=2080 — just past the splice tail so the
        // client treats it as the next-expected reliable bunch.  Each
        // fuzz bunch consumes one chSeq.
        static thread_local uint32_t pc_ch_reliable_seq = 2080;
        int sent_ok = 0;

        // Helper: count bits SerializeIntPacked needs for value v
        auto sip_bit_count = [](uint32_t v) -> size_t {
            if (v == 0) return 8;
            size_t bits = 0;
            while (v > 0) { bits += 8; v >>= 7; }
            return bits;
        };

        for (size_t fi = 0; fi < kFuzzCount; ++fi) {
            const uint16_t field_handle    = kFuzzVariants[fi].handle;
            const bool     has_header_bit  = kFuzzVariants[fi].has_header_bit;
            // Wire value is handle+1 (handle is 0-based; 0 = terminator)
            const uint32_t wire_handle     = static_cast<uint32_t>(field_handle) + 1u;

            // ── Compute sizes (sub_1444E55B0 format) ──
            // Inner field payload = Pawn NetGUID (128 bits)
            const size_t inner_field_payload_bits = 128;
            const size_t handle_sip_bits = sip_bit_count(wire_handle);
            const size_t size_sip_bits   = sip_bit_count(static_cast<uint32_t>(inner_field_payload_bits));
            const size_t terminator_sip_bits = 8;  // SIP(0) is 1 byte = 8 bits

            // Outer content block payload (the "v98 bits" from RE):
            //   [1 bit header (optional)] + [SIP handle+1] + [SIP size] + [128 payload] + [SIP 0 term]
            const size_t outer_payload_bits = (has_header_bit ? 1u : 0u)
                                            + handle_sip_bits
                                            + size_sip_bits
                                            + inner_field_payload_bits
                                            + terminator_sip_bits;
            const size_t outer_size_varint_bits = sip_bit_count(static_cast<uint32_t>(outer_payload_bits));

            // Content block envelope: 2 bits + size_varint + outer payload
            const size_t total_bdb = 2 + outer_size_varint_bits + outer_payload_bits;

            // Build the ch=3 reliable bunch
            uint8_t bunch_buf[256] = {};
            size_t bb = 0;
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bControl=0
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bIsReplicationPaused=0
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 1, 1); // bReliable=1
            ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb, 3);    // ChIdx=3
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bHasPME=0
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bHasMBG=0
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1); // bPartial=0

            const uint32_t this_chseq = pc_ch_reliable_seq++;
            // PM20 (2026-04-28): FIX — ChSeq is 10 bits, not 12.
            // (Dead code: PM19 disabled the only caller — kept consistent.)
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                            this_chseq & 0x3FF, 10);                 // ChSeq (10-bit)
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 1, 1); // ChName.bIsHardcoded=1
            ue5::write_sip (bunch_buf, sizeof(bunch_buf), bb, 102);  // EName=102 (NAME_Actor)

            // BunchDataBits = total content (2 envelope + size varint + inner)
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                            total_bdb, 13);

            // ── Content block header ──
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1);  // bHasRepLayout=0
            ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 1, 1);  // bIsActor=1

            // Outer payload size as SIP varint
            ue5::write_sip(bunch_buf, sizeof(bunch_buf), bb,
                            static_cast<uint32_t>(outer_payload_bits));

            // ── Outer payload (sub_1444E5420 + sub_1444E55B0 format) ──

            // [1 bit] header (skipped by sub_1444E5420 lines 21-28)
            // Optional per fuzz variant.
            if (has_header_bit) {
                ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb, 0, 1);
            }

            // [SIP] handle+1 (0-based handle, +1 because 0 = terminator)
            ue5::write_sip(bunch_buf, sizeof(bunch_buf), bb, wire_handle);

            // [SIP] field payload bits (= 128 for FIntrepidNetGUID)
            ue5::write_sip(bunch_buf, sizeof(bunch_buf), bb,
                            static_cast<uint32_t>(inner_field_payload_bits));

            // [N bits] field payload — FIntrepidNetGUID (128 bits)
            for (int i = 0; i < 8; ++i) {
                ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                                (pawn_obj_id >> (i * 8)) & 0xFF, 8);
            }
            for (int i = 0; i < 4; ++i) {
                ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                                (pawn_server_id >> (i * 8)) & 0xFF, 8);
            }
            for (int i = 0; i < 4; ++i) {
                ue5::write_bits(bunch_buf, sizeof(bunch_buf), bb,
                                (pawn_randomizer >> (i * 8)) & 0xFF, 8);
            }
// -- C_Y_B . CoA --

            // [SIP] 0 terminator — REQUIRED per sub_1444E55B0 line 182
            // (otherwise the loop never exits gracefully and the bunch
            // is treated as malformed → silent drop)
            ue5::write_sip(bunch_buf, sizeof(bunch_buf), bb, 0);

            const size_t bunch_bits = bb;

            // Wrap in UDP packet
            uint8_t buf[1024] = {};
            size_t off = 0;
            write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);
            ue5::write_bits(buf, sizeof(buf), off, 1,    1);
            ue5::write_bits(buf, sizeof(buf), off, 1023, 10);
            ue5::write_bits(buf, sizeof(buf), off, 0,    1);
            for (size_t i = 0; i < bunch_bits; ++i) {
                int bit = (bunch_buf[i >> 3] >> (i & 7)) & 1;
                ue5::write_bits(buf, sizeof(buf), off, bit, 1);
            }
            // ── EXTRA SENTINEL BIT (AoC requirement, see send_keepalive ~5659) ──
            // AoC client's PacketHandler expects an extra '1' bit BEFORE the
            // standard add_termination.  Without it the handler misprocesses
            // and reports "Received packet with 0's in last byte" (the
            // misleading default fault label).  Splice packets bring this
            // bit in their captured payload; native packets must add it.
            ue5::write_bits(buf, sizeof(buf), off, 1, 1);
            size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);
            const uint16_t sent_seq = cs.out_seq;
            cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

            int sent = send_to_client_impl(buf, pkt_len, addr);
            if (sent > 0) {
                spdlog::warn("[GameServer] >> ★ NATIVE ClientRestart FUZZ "
                              "[{}/{}]: hdr_bit={} handle={} (wire={}) chSeq={} bunch_bits={} pkt={}B seq={}",
                              fi+1, kFuzzCount,
                              has_header_bit ? 1 : 0, field_handle, wire_handle,
                              this_chseq, bunch_bits, pkt_len, sent_seq);
                ++sent_ok;
            }
        }
        spdlog::warn("[GameServer] FUZZ COMPLETE: sent {}/{} ClientRestart variants "
                      "(pawn obj={} srv={} rnd=0x{:08x})",
                      sent_ok, kFuzzCount, pawn_obj_id, pawn_server_id, pawn_randomizer);
        return sent_ok > 0;
#endif  // ── END HISTORIC FUZZ BODY ──

    /// Send UDP packet to a client (original implementation, renamed so
    /// the IGameServerHost virtual above can call into it without
    /// creating a recursive cycle).
    int send_to_client_impl(const uint8_t* data, size_t len, const sockaddr_in& dest) {
        // Diagnostic: every outgoing packet logs last4 so we can correlate
        // with client's "0's in last byte" warnings.
        if (len > 0) {
            spdlog::debug("[GameServer] SND len={} last4=[{:02x} {:02x} {:02x} {:02x}]",
                          len,
                          len >= 4 ? data[len-4] : 0, len >= 3 ? data[len-3] : 0,
                          len >= 2 ? data[len-2] : 0, data[len-1]);
        }
        if (len > 0 && data[len - 1] == 0) {
            spdlog::error("[GameServer] BUG: ZERO LAST BYTE in outgoing packet! len={} last8=[{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}]",
                          len,
                          len >= 8 ? data[len-8] : 0, len >= 7 ? data[len-7] : 0,
                          len >= 6 ? data[len-6] : 0, len >= 5 ? data[len-5] : 0,
                          len >= 4 ? data[len-4] : 0, len >= 3 ? data[len-3] : 0,
                          len >= 2 ? data[len-2] : 0, data[len-1]);
            spdlog::error("[GameServer]   full hex: {}", ue5::hex_dump(data, len, 128));
        }
        const int sent = sendto(sock_, reinterpret_cast<const char*>(data),
                                static_cast<int>(len), 0,
                                reinterpret_cast<const sockaddr*>(&dest),
                                sizeof(dest));
// (C_Y_B + C.O.A)

        // ── S>C counter (symmetric with << C>S #N logging) ──────────────
        // Every outgoing UDP packet gets a sequence-independent monotonic
        // counter so users can see the full S>C stream alongside C>S.
        // Kept at info level so keepalives show up; if this is too noisy
        // for long sessions we can move to debug later.
        if (sent > 0) {
            const uint32_t n = ++sc_pkt_counter_;
            char addr_buf[32] = {};
            inet_ntop(AF_INET, &dest.sin_addr, addr_buf, sizeof(addr_buf));
            spdlog::info("[GameServer] >> S>C #{} to {}:{} {}B "
                         "first8=[{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}] "
                         "last4=[{:02x} {:02x} {:02x} {:02x}]",
                         n, addr_buf, ntohs(dest.sin_port), sent,
                         len >= 1 ? data[0] : 0, len >= 2 ? data[1] : 0,
                         len >= 3 ? data[2] : 0, len >= 4 ? data[3] : 0,
                         len >= 5 ? data[4] : 0, len >= 6 ? data[5] : 0,
                         len >= 7 ? data[6] : 0, len >= 8 ? data[7] : 0,
                         len >= 4 ? data[len-4] : 0, len >= 3 ? data[len-3] : 0,
                         len >= 2 ? data[len-2] : 0, data[len-1]);
        } else if (sent < 0) {
            spdlog::warn("[GameServer] >> S>C SEND FAILED (len={}, WSAGetLastError={})",
                         len,
#ifdef _WIN32
                         WSAGetLastError()
#else
                         errno
#endif
                         );
        }
        return sent;
    }

    // Monotonic counter for outgoing UDP packets (informational only —
    // not the per-channel chSeq or per-client out_seq).  Wraps at 2^32.
    std::atomic<uint32_t> sc_pkt_counter_{0};

    // ── DNS resolution (relay mode) ─────────────────────────────────────
    bool resolve_relay_target() {
        std::string host = config_.relay_target;
        int port = config_.port;
        auto colon = host.rfind(':');
        if (colon != std::string::npos) {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        }
        spdlog::info("[GameServer] Resolving '{}':{} ...", host, port);
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
        if (rc != 0 || !result) {
            spdlog::error("[GameServer] DNS failed for '{}': {}", host, gai_strerror(rc));
            return false;
        }
        // Defensive: hints.ai_family=AF_INET should guarantee IPv4 only, but
        // some libc implementations have returned AF_INET6 under odd configs.
        // Reject anything that isn't IPv4 rather than reinterpret-cast into UB.
        if (result->ai_family != AF_INET) {
            spdlog::error("[GameServer] DNS for '{}' returned non-IPv4 family {}",
                          host, result->ai_family);
            freeaddrinfo(result);
            return false;
        }
        upstream_addr_ = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
        freeaddrinfo(result);
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &upstream_addr_.sin_addr, ip_str, sizeof(ip_str));
        spdlog::info("[GameServer] Resolved: {} -> {}:{}", config_.relay_target, ip_str, ntohs(upstream_addr_.sin_port));
        return true;
    }

    // ── Handshake handling (emulation mode) ─────────────────────────────

    void handle_packet(const uint8_t* data, size_t len,
                       const sockaddr_in& client_addr) {
        ue5_hs::HandshakePacket pkt;
        if (!ue5_hs::parse(data, len, pkt)) {
            spdlog::warn("[GameServer] Failed to parse packet ({}B)", len);
            return;
        }

        std::string key = addr_key(client_addr);

        if (!pkt.is_handshake) {
            handle_game_data(data, len, client_addr, key);
            return;
        }

        spdlog::info("[GameServer] Handshake from {} — Type={} ({}) Count={} MinV={} CurV={} NetVer=0x{:08x}",
                     key, pkt.packet_type,
                     pkt.packet_type == 0 ? "Initial" :
                     pkt.packet_type == 1 ? "Challenge" :
                     pkt.packet_type == 2 ? "Response" :
                     pkt.packet_type == 3 ? "Ack" : "?",
                     pkt.sent_count, pkt.min_version, pkt.cur_version,
                     pkt.network_version);

        if (pkt.has_custom_ext) {
            spdlog::info("[GameServer]   CharID: {} Token: {}", pkt.char_id, pkt.token);
        }

        switch (pkt.packet_type) {
        case ue5_hs::Initial:
            handle_initial(pkt, client_addr, key);
            break;
        case ue5_hs::Response:
            handle_response(pkt, client_addr, key);
            break;
        default:
            spdlog::warn("[GameServer]   Unexpected handshake type {} from client", pkt.packet_type);
            break;
        }
    }

    void handle_initial(const ue5_hs::HandshakePacket& pkt,
                        const sockaddr_in& client_addr,
                        const std::string& key) {
        std::lock_guard<std::mutex> lk(client_mu_);

        auto& cs = clients_[key];
        cs.addr = client_addr;
        cs.init_from(pkt);
        cs.last_activity = std::chrono::steady_clock::now();

        // Generate challenge cookie
        cs.challenge_timestamp = elapsed_seconds();
        cs.secret_id = 1;  // Real AoC server uses secret_id=1
        cs.server_sent_count++;
        generate_cookie(cs.challenge_timestamp, key, cs.challenge_cookie);

        // Fix #36b: In replay mode, patch cookie[0:2] = replay initial_seq.
        // Real AoC server sends the same cookie in both Challenge and Ack.
        // UE5/AoC client reads InitialPacketSequence from cookie[0:2] of the
        // Ack (and/or Challenge).  Patching here ensures both packets carry
        // the correct value so client InReliable[ch] = initial_seq & 0xFFF.
        if (replay_data_) {
            uint16_t target_seq = replay_data_->initial_seq;
            std::memcpy(cs.challenge_cookie, &target_seq, 2);
        }

        // Generate Intrepid extension (94 bytes) — echoes CharID+Token
        // from Initial plus spawn coordinates and server random data.
        // Persisted in cs.ext_data so the Ack reuses the same bytes.
        if (pkt.has_custom_ext) {
            // Default Lyneth spawn coordinates from MITM capture
            cs.ext_data.generate(pkt.char_id_raw, pkt.token_raw,
                                 -777664.5, 616578.7, 15952.0);
        }

        // Build Challenge packet (153B with extension, 72B without)
        uint8_t buf[512] = {};
        auto tmpl = cs.as_template();
        int trail_pad = cs.ext_data.present ? 15 : 28;
        size_t pkt_len = ue5_hs::build(buf, sizeof(buf), tmpl,
                                         ue5_hs::Challenge,
                                         cs.server_sent_count,
                                         cs.secret_id,
                                         cs.challenge_timestamp,
                                         cs.challenge_cookie,
                                         &cs.ext_data,
                                         trail_pad);

        cs.phase = ClientState::CHALLENGE_SENT;

        spdlog::info("[GameServer] >> Sending Challenge to {} ({}B, ts={:.6f})", key, pkt_len, cs.challenge_timestamp);
        spdlog::debug("[GameServer]    hex: {}", ue5::hex_dump(buf, pkt_len, 256));

        int sent = send_to_client(buf, pkt_len, client_addr);
        if (sent <= 0) {
            spdlog::error("[GameServer]    Send Challenge FAILED");
        }

        ProxyLogger::instance().log_udp("S>C", key, buf, pkt_len, "Challenge");
    }
// -- C_Y_B & CoA --

    void handle_response(const ue5_hs::HandshakePacket& pkt,
                         const sockaddr_in& client_addr,
                         const std::string& key) {
        std::lock_guard<std::mutex> lk(client_mu_);

        auto it = clients_.find(key);
        if (it == clients_.end()) {
            spdlog::warn("[GameServer]   Response from unknown client {}", key);
            return;
        }

        auto& cs = it->second;

        bool cookie_match = std::memcmp(pkt.cookie, cs.challenge_cookie,
                                        ue5_hs::COOKIE_SIZE) == 0;
        bool ts_match = (pkt.timestamp == cs.challenge_timestamp);

        spdlog::info("[GameServer]   Response cookie_match={} ts_match={} (ts={:.6f} vs {:.6f})",
                     cookie_match, ts_match, pkt.timestamp, cs.challenge_timestamp);

        // Build Ack cookie.
        // Real AoC server reuses the Challenge cookie for the Ack (MITM
        // capture confirms all 20 bytes identical).  The client reads two
        // uint16 InitialPacketSequence values from cookie[0..1] (ServerSeq)
        // and cookie[2..3] (ClientSeq).  NOT int32 from [0..3]/[4..7]!
        // Evidence: cookie ef 95 7f ca → uint16 LE [0..1]=0x95EF→5615,
        //           [2..3]=0xCA7F→2687 matches client's first Seq=2687.
        uint8_t ack_cookie[ue5_hs::COOKIE_SIZE];
        std::memcpy(ack_cookie, cs.challenge_cookie, ue5_hs::COOKIE_SIZE);

        // Derive initial sequences from cookie bytes.
        // UE5/AoC reads two uint16 from cookie[0..1] and cookie[2..3],
        // not two int32 from [0..3] and [4..7].
        uint16_t raw_server_seq = 0, raw_client_seq = 0;
        std::memcpy(&raw_server_seq, ack_cookie, 2);
        std::memcpy(&raw_client_seq, ack_cookie + 2, 2);

        // Fix #36: In replay mode, override the server initial sequence to match
        // the captured session.  UE5/AoC initializes InReliable[ch] for ALL
        // channels to (initial_server_seq & 0xFFF) at connection setup.
        // The captured session had initial_seq=14265 → in_reliable_base=1977,
        // so every channel-open bunch in the replay has ChSequence=1978.
        // If our HMAC cookie happens to give a different in_reliable_base,
        // the client queues all open bunches as "out of order" and channel 3+
        // never open → no actors, no buildings, no character.
        // Solution: force raw_server_seq = replay_data_->initial_seq so that
        // in_reliable_base = replay_data_->initial_seq & 0xFFF = 1977 always.
        if (replay_data_) {
            raw_server_seq = replay_data_->initial_seq;
            std::memcpy(ack_cookie, &raw_server_seq, 2);
            spdlog::info("[GameServer]   [Replay] Overriding initial_seq={} (in_reliable_base={})",
                         raw_server_seq, raw_server_seq & 0xFFF);
        }

        cs.server_sent_count++;

        // Build Ack (Timestamp = -1.0 signals handshake complete)
        // Include Intrepid extension (same data as Challenge) + 9B trailing pad
        uint8_t buf[512] = {};
        auto tmpl = cs.as_template();
        double ack_ts = -1.0;
        int trail_pad = cs.ext_data.present ? 9 : 28;
        size_t pkt_len = ue5_hs::build(buf, sizeof(buf), tmpl,
                                         ue5_hs::Ack,
                                         cs.server_sent_count,
                                         cs.secret_id,
                                         ack_ts,
                                         ack_cookie,
                                         &cs.ext_data,
                                         trail_pad);

        cs.phase = ClientState::HANDSHAKE_COMPLETE;
        int16_t server_seq = static_cast<int16_t>(raw_server_seq & ClientState::SEQ_MASK);
        int16_t client_seq = static_cast<int16_t>(raw_client_seq & ClientState::SEQ_MASK);
        cs.init_sequences(server_seq, client_seq);

        // Session F: register this client with LiveWorld so its
        // SessionRegistry + VisibilityManager know about the new connection.
        // Subsequent data packets get routed through LiveWorld's dispatcher
        // (see maybe_feed_live_world()).  No-op if --enable-live-world is off.
        if (live_world_) {
            live_world_->on_client_connected(key);

            // Session G: populate the live world with the player's actors so
            // BroadcastManager has something to fan out to UdpPacketEmitter.
            // Runs only when session_g_spawn_actors is on (default).
            if (config_.session_g_spawn_actors) {
                spawn_session_g_actors_for(key);
            }
        }

        spdlog::info("[GameServer] >> Sending Ack to {} ({}B) — HANDSHAKE COMPLETE!", key, pkt_len);
        spdlog::info("[GameServer]   ServerSeq={} ClientSeq={} (raw: 0x{:04x} / 0x{:04x})",
                     server_seq, client_seq, raw_server_seq, raw_client_seq);
        spdlog::debug("[GameServer]    hex: {}", ue5::hex_dump(buf, pkt_len, 256));

        int sent = send_to_client(buf, pkt_len, client_addr);
        if (sent <= 0) {
            spdlog::error("[GameServer]    Send Ack FAILED");
        }

        ProxyLogger::instance().log_udp("S>C", key, buf, pkt_len, "Ack");

        spdlog::info("[GameServer] ====================================");
        spdlog::info("[GameServer]  CLIENT CONNECTED!");
        spdlog::info("[GameServer]  char_id: {}", cs.char_id);
        spdlog::info("[GameServer]  token:   {}", cs.token);
        spdlog::info("[GameServer] ====================================");
    }

    void handle_game_data(const uint8_t* data, size_t len,
                          const sockaddr_in& client_addr,
                          const std::string& key) {
        // ── Session F hook ───────────────────────────────────────────────
        // Observe every post-handshake data packet so LiveWorld's session
        // activity + stats reflect real traffic.  Real wire-format opcode
        // extraction → dispatch happens in Session G once the
        // ParsedBunch/ParsedPacket pipeline is wired into OpcodeDispatcher.
        if (live_world_) {
            live_world_->on_data_packet_observed(key);
        }

        // ── Strip termination and compute effective bits ────────────────
        size_t eff_bits = ue5::strip_termination(data, len);
        // Outer layer: MagicHeader(32) + SessionID(2) + ClientID(3) + HandshakeBit(1) = 38 bits
        constexpr size_t OUTER_BITS = 38;
        size_t off = OUTER_BITS;

        if (eff_bits < off + 64) { // need packed header (32) + 1 history word (32)
            spdlog::warn("[GameServer] Data packet too short from {}: {}B ({} eff bits)", key, len, eff_bits);
            return;
        }

        // ── FNetPacketNotify packed header ──────────────────────────────
        uint32_t packed = static_cast<uint32_t>(ue5::read_bits(data, len, off, 32));
        uint16_t seq        = (packed >> 18) & 0x3FFF;
        uint16_t acked_seq  = (packed >> 4)  & 0x3FFF;
        uint16_t hist_count = (packed & 0x0F) + 1; // stored as count-1

        // Read history words
        std::vector<uint32_t> hist(hist_count);
        for (uint16_t i = 0; i < hist_count; ++i)
            hist[i] = static_cast<uint32_t>(ue5::read_bits(data, len, off, 32));

        // ── AoC custom field (48 bits / 6 bytes, SAME both directions) ──
        // AoC inserts a custom field BEFORE the standard UE5 PacketInfo.
        // Both C>S and S>C use 6 constant bytes per session, derived from
        // IntrepidExtData[0:6] exchanged during the handshake.
        constexpr int CF_BYTES = ClientState::CUSTOM_FIELD_BYTES;
        constexpr int CF_BITS  = CF_BYTES * 8;
        uint8_t custom_field[6] = {};
        if (off + CF_BITS <= eff_bits) {
            for (int i = 0; i < CF_BYTES; ++i)
                custom_field[i] = static_cast<uint8_t>(ue5::read_bits(data, len, off, 8));
        }

        // ── PacketInfo (AoC variant, AFTER AoC custom field) ──────────
        // C>S with hasPktInfo=0 carries NO hasServerFrameTime bit (stock
        // UE5).  Reading one misaligned every burst packet by 1 bit — see
        // protocol/wire/packet_info.h and libs/aocnet/tests/test_cs_packet_info.cpp.
        const auto pinfo = aoc::protocol::wire::read_packet_info(
            data, len, off, aoc::protocol::wire::Direction::ClientToServer);
        const bool has_pkt_info  = pinfo.has_pkt_info;
        const bool has_srv_frame = pinfo.has_srv_frame;
// [Cyb ~ C.O.A]

        size_t bunch_start = off;
        size_t bunch_bits  = (eff_bits > off) ? (eff_bits - off) : 0;

        spdlog::info("[GameServer] DATA {} — {}B Seq={} AckSeq={} Hist={} PktInfo={} SrvBit={} BunchBits={} off={}",
                     key, len, seq, acked_seq, hist_count, has_pkt_info, has_srv_frame, bunch_bits, bunch_start);

        int nmt_type = -1;
        std::vector<int> nmt_types;
        std::vector<float> server_move_ts_list; // Monotonic frame timestamp (seconds) per ServerMove bunch
        bool client_closed = false;             // control-channel Close seen in this packet

        // ── Dump bunch bits (binary + hex) for analysis ────────────────
        if (bunch_bits > 0) {
            // Binary dump (first 400 bits)
            std::string bit_str;
            size_t show = std::min(bunch_bits, size_t(400));
            for (size_t i = 0; i < show; ++i) {
                size_t bidx = (bunch_start + i) / 8;
                int boff = (bunch_start + i) % 8;
                if (bidx < len)
                    bit_str += ((data[bidx] >> boff) & 1) ? '1' : '0';
                if ((i + 1) % 8 == 0) bit_str += ' ';
            }
            spdlog::info("[GameServer]   Bunch bits: {}", bit_str);

            // Hex dump
            std::vector<uint8_t> bunch_hex;
            size_t tmp = bunch_start;
            while (tmp + 8 <= eff_bits)
                bunch_hex.push_back(static_cast<uint8_t>(ue5::read_bits(data, len, tmp, 8)));
            spdlog::info("[GameServer]   Bunch hex ({}B): {}", bunch_hex.size(),
                         ue5::hex_dump(bunch_hex.data(), bunch_hex.size(), 256));

            // ── Every bunch in the packet ────────────────────────────────
            // wire::walk_bunches owns header parsing and only stops on an
            // unreadable header — not after an RPC bunch, which the old loop
            // did (dropping e.g. the 2nd of two SULVs and a trailing ServerMove).
            size_t parsed_to = bunch_start;
            aoc::protocol::wire::walk_bunches(data, len, bunch_start, eff_bits,
                [&](const aoc::protocol::wire::ParsedBunch& hdr) {
                    float sm_ts = 0.0f;
                    int nmt = try_parse_bunch(data, len, hdr, &sm_ts, &key, &client_addr);
                    if (sm_ts > 0.0f) server_move_ts_list.push_back(sm_ts);
                    if (nmt >= 0) nmt_types.push_back(nmt);
                    if (hdr.channel == 0 && hdr.is_control && hdr.is_close) client_closed = true;
                    parsed_to = hdr.data_start_bit + hdr.bunch_data_bits;
                });
            // A clean packet leaves only the AoC sentinel bit.
            if (eff_bits > parsed_to + 8) {
                std::vector<uint8_t> rest;
                size_t tb = parsed_to;
                while (tb + 8 <= eff_bits && rest.size() < 16)
                    rest.push_back(static_cast<uint8_t>(ue5::read_bits(data, len, tb, 8)));
                spdlog::warn("[C>S-PARSE] {} unparsed bits at bit {} of {} — next bytes: {}",
                             eff_bits - parsed_to, parsed_to, eff_bits,
                             ue5::hex_dump(rest.data(), rest.size(), 64));
            }
            if (!nmt_types.empty()) {
                nmt_type = nmt_types.back(); // last valid NMT type for compat
                if (nmt_types.size() > 1)
                    spdlog::info("[GameServer]   Multi-bunch: found {} NMT types", nmt_types.size());
            }
        }

        if (client_closed) {
            handle_client_close(key);
            return;
        }

        // ── Update state & respond based on NMT type ────────────────────
        {
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(key);
            if (it != clients_.end()) {
                auto& cs = it->second;
                cs.in_ack_seq = seq;
                cs.out_ack_seq = acked_seq;
                cs.pkt_recv_count++;
                cs.last_activity = std::chrono::steady_clock::now();

                // ── Capture AoC custom field from first data packet ────
                // The C>S custom field was already read during packet parsing.
                // For S>C we echo the client's C>S field.  The replay file's
                // field is from a different session and the client rejects it
                // ("FaultDisconnect,NotRecoverable,ZeroLastByte").
                if (!cs.custom_field_captured) {
                    // Both C>S and S>C use same 6-byte (48-bit) custom field
                    // derived from IntrepidExtData[0:6] sent during handshake.
                    std::memcpy(cs.custom_field, custom_field, ClientState::CUSTOM_FIELD_BYTES);
                    spdlog::warn("[GameServer] ★ Custom field ({}B): "
                                 "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                                 ClientState::CUSTOM_FIELD_BYTES,
                                 cs.custom_field[0], cs.custom_field[1], cs.custom_field[2],
                                 cs.custom_field[3], cs.custom_field[4], cs.custom_field[5]);
                    cs.custom_field_captured = true;
                }

                // NMT game-level handshake state machine
                // In replay mode, we MUST complete the NMT negotiation
                // before launching the replay thread.  The replay data
                // contains NMT packets from a different session that the
                // client would reject.
                //
                // Fix #17: Process ALL NMT types from multi-bunch packets.
                // The client may send Hello+Login in one packet (C>S #4).
                if (nmt_types.empty() && nmt_type >= 0)
                    nmt_types.push_back(nmt_type);

                if (config_.native_mode) {
                    // ── PATH B: NATIVE / AUTHORITATIVE-SERVER MODE ─────
                    // M1.0: dispatch NMT handshake (same sequence as replay
                    // mode), and once NMT completes, hand control to the
                    // NativeConnectSequencer instead of a replay thread.
                    for (int cur_nmt : nmt_types) {
                        if (cs.nmt_state < 4) {
                            if (cur_nmt == 0 && cs.nmt_state == 0) {
                                spdlog::info("[GameServer] ★ NMT_Hello → NMT_Challenge [NATIVE]");
                                send_nmt_challenge(cs, client_addr, key);
                                cs.nmt_state = 1;
                            } else if (cur_nmt == 5 && cs.nmt_state == 1) {
                                spdlog::info("[GameServer] ★ NMT_Login → NMT_Welcome [NATIVE]");
                                send_nmt_welcome(cs, client_addr, key);
                                cs.nmt_state = 2;
                            } else if (cur_nmt == 4 && cs.nmt_state >= 2) {
                                spdlog::info("[GameServer] ★ NMT_NetSpeed [NATIVE]");
                                send_keepalive(cs, client_addr, key);
                                cs.nmt_state = 3;
                            } else if ((cur_nmt == 9 || cur_nmt == 18) && cs.nmt_state >= 2) {
                                spdlog::info("[GameServer] ★★★ NMT_{} — NATIVE SEQUENCER READY ★★★",
                                             cur_nmt);
                                send_keepalive(cs, client_addr, key);
                                cs.nmt_state = 4;
                                cs.phase = ClientState::CONNECTED;

                                // Stop previous sequencer if any (reconnect case).
                                if (native_sequencer_) {
                                    native_sequencer_->stop();
                                    native_sequencer_.reset();
                                }
                                // M1.4.d: reset map-loaded flag so the new
                                // sequencer waits for this session's LoadMap
                                // signal (not a stale value from a previous
                                // reconnect).
                                native_map_loaded_.store(false);
// [C O A :: C-Y-B]

                                // M1.4.e — HYBRID MODE: if --replay is ALSO
                                // provided alongside --native, start the
                                // proven replay_loop instead of the native
                                // sequencer for bootstrap.  This uses the
                                // existing replay infrastructure to emit
                                // the first N captured packets (#0..N-1)
                                // which empirically get the client into
                                // the world.  Native Maintain resumes after.
                                //
                                // Cap controlled by --replay-max-packets (CLI)
                                // or config_.replay_max_packets (default
                                // 29010 if unset; recommend 100 for
                                // minimum-world-entry per user test).
                                //
                                // Path B roadmap (see
                                // docs/NATIVE-EMISSION-ARCHITECTURE.md):
                                // progressive replacement — each pkt
                                // promoted from splice → native as its
                                // emitter class becomes byte-identical
                                // to captured fixture.
                                const bool replay_loaded =
                                    replay_data_ && !replay_data_->packets.empty();
                                const bool start_replay_thread =
                                    replay_loaded && !config_.disable_replay_loop;

                                if (start_replay_thread) {
                                    spdlog::warn("[GameServer] ★ HYBRID MODE: "
                                                 "replay_loop will emit first "
                                                 "{} packets (native NMT + "
                                                 "replay body)",
                                                 config_.replay_max_packets > 0
                                                     ? config_.replay_max_packets
                                                     : replay_data_->packets.size());
                                    // Kick off existing replay_loop — same
                                    // thread model as pure replay mode.
                                    replay_active_.store(false);
                                    replay_map_loaded_.store(false);
                                    if (replay_thread_.joinable())
                                        replay_thread_.join();
                                    replay_active_.store(true);
                                    {
                                        std::string rkey = key;
                                        sockaddr_in raddr = client_addr;
                                        replay_thread_ = std::thread(
                                            [this, rkey, raddr]() {
                                                replay_loop(rkey, raddr);
                                            });
                                    }
                                    spdlog::warn("[GameServer] replay_loop "
                                                 "thread launched (hybrid mode)");

                                    // ── HYBRID+NATIVE (2026-04-26): in --native
                                    // mode, ALWAYS start the NativeConnectSequencer
                                    // alongside replay.  Replay handles the world
                                    // bootstrap (which our BootstrapEmitter doesn't
                                    // yet implement); native sequencer handles
                                    // ongoing emission (opcode-3, PC, props, pawn).
                                    spdlog::warn("[GameServer] HYBRID+NATIVE: "
                                                 "replay + native sequencer running "
                                                 "in parallel (replay handles world "
                                                 "bootstrap, native handles ongoing "
                                                 "emission).");
                                    native_sequencer_ = std::make_unique<aoc::net::NativeConnectSequencer>(
                                        *this, key, client_addr);
                                    native_sequencer_->start();
                                } else if (replay_loaded && config_.disable_replay_loop) {
                                    // ── PURE-NATIVE (Road A — Phase B.0) ──
                                    // Replay file IS loaded (so the
                                    // WorldBootstrapEmitter's Splice rows
                                    // can pull captured bytes via
                                    // send_captured_packet), but NO replay
                                    // thread is launched.  The native
                                    // sequencer is the sole driver of the
                                    // post-NMT bootstrap stream.
                                    spdlog::warn("[GameServer] ★ PURE-NATIVE MODE: "
                                                 "replay loaded ({} packets) but "
                                                 "thread DISABLED — sequencer drives "
                                                 "bootstrap via WorldBootstrapEmitter",
                                                 replay_data_->packets.size());
                                    native_sequencer_ = std::make_unique<aoc::net::NativeConnectSequencer>(
                                        *this, key, client_addr);
                                    native_sequencer_->start();
                                } else {
                                    // No replay loaded at all — pure native,
                                    // sequencer plan can only run NativeXxx
                                    // rows; Splice rows will log a warning
                                    // and skip.
                                    spdlog::warn("[GameServer] NATIVE MODE: no "
                                                 "replay loaded — Splice rows in "
                                                 "the bootstrap plan will be skipped");
                                    native_sequencer_ = std::make_unique<aoc::net::NativeConnectSequencer>(
                                        *this, key, client_addr);
                                    native_sequencer_->start();
                                    spdlog::warn("[GameServer] NativeConnectSequencer started for {}", key);
                                }
                            }
                        }
                    }
                    // Keepalive if state didn't advance
                    if (cs.nmt_state < 4 && !nmt_types.empty()) {
                        bool any_advanced = false;
                        for (int t : nmt_types) {
                            if ((t == 0 && cs.nmt_state >= 1) || (t == 5 && cs.nmt_state >= 2) ||
                                (t == 4 && cs.nmt_state >= 3)) {
                                any_advanced = true;
                                break;
                            }
                        }
                        if (!any_advanced) send_keepalive(cs, client_addr, key);
                    }
// C-Y-B with CoA

                    // M1.4.d — Fix #36 equivalent: detect when the client
                    // finishes LoadMap().  Once NMT is complete (state >= 4),
                    // the FIRST packet the client sends with any bunch bits
                    // and no NMT opcodes is the "game thread running" signal.
                    // Before this, PC ActorOpen would be dropped because the
                    // client's game thread is still loading the world.
                    if (cs.nmt_state >= 4 && bunch_bits > 0 && nmt_types.empty()
                        && !native_map_loaded_.load()) {
                        native_map_loaded_.store(true);
                        spdlog::warn("[GameServer] *** MAP LOADED [NATIVE]: "
                                     "client sent game data on non-control ch "
                                     "— sequencer advancing to SendBootstrap");
                    }
                    // M1.4.e — Hybrid mode: the replay_loop (if started as
                    // the hybrid bootstrap) waits on replay_map_loaded_,
                    // not native_map_loaded_.  Set both so either path
                    // unblocks the emission thread.
                    if (cs.nmt_state >= 4 && bunch_bits > 0 && nmt_types.empty()
                        && replay_data_ && !replay_data_->packets.empty()
                        && !replay_map_loaded_.load()) {
                        replay_map_loaded_.store(true);
                        spdlog::warn("[GameServer] *** MAP LOADED [HYBRID]: "
                                     "replay_loop unblocked");
                    }
                } else if (replay_data_ && !replay_data_->packets.empty()) {
                    // ── REPLAY MODE ────────────────────────────────────
                    for (int cur_nmt : nmt_types) {
                        if (cs.nmt_state >= 4 && !replay_active_.load()) {
                            // NMT complete → launch replay thread
                            replay_active_.store(true);
                            std::string rkey = key;
                            sockaddr_in raddr = client_addr;
                            replay_thread_ = std::thread(
                                [this, rkey, raddr]() { replay_loop(rkey, raddr); });
                            spdlog::warn("[Replay] Replay thread launched for {} (after NMT)", key);
                            break;
                        } else if (cs.nmt_state < 4) {
                            if (cur_nmt == 0 && cs.nmt_state == 0) {
                                spdlog::info("[GameServer] ★ NMT_Hello → NMT_Challenge [REPLAY]");
                                send_nmt_challenge(cs, client_addr, key);
                                cs.nmt_state = 1;
                            } else if (cur_nmt == 5 && cs.nmt_state == 1) {
                                spdlog::info("[GameServer] ★ NMT_Login → NMT_Welcome [REPLAY]");
                                send_nmt_welcome(cs, client_addr, key);
                                cs.nmt_state = 2;
                            } else if (cur_nmt == 4 && cs.nmt_state >= 2) {
                                spdlog::info("[GameServer] ★ NMT_NetSpeed [REPLAY]");
                                send_keepalive(cs, client_addr, key);
                                cs.nmt_state = 3;
                            } else if (cur_nmt == 9 && cs.nmt_state >= 2) {
                                spdlog::info("[GameServer] ★★★ NMT_Join — REPLAY READY ★★★");
                                send_keepalive(cs, client_addr, key);
                                cs.nmt_state = 4;
                                cs.phase = ClientState::CONNECTED;
                                // Launch replay. Reset load flag (Fix #36).
                                replay_active_.store(false);
                                replay_map_loaded_.store(false); // reset for new session
                                if (replay_thread_.joinable()) replay_thread_.join();
                                replay_active_.store(true);
                                {
                                    std::string rkey = key;
                                    sockaddr_in raddr = client_addr;
                                    replay_thread_ = std::thread(
                                        [this, rkey, raddr]() { replay_loop(rkey, raddr); });
                                    spdlog::warn("[Replay] Replay thread launched (NMT_Join)");
                                }
                            } else if (cur_nmt == 18 && cs.nmt_state >= 2) {
                                // Fix #33: AoC client sends GameSpecific(18) instead of Join(9)
                                spdlog::info("[GameServer] ★★★ NMT_GameSpecific(18) — REPLAY READY ★★★");
                                send_keepalive(cs, client_addr, key);
                                cs.nmt_state = 4;
                                cs.phase = ClientState::CONNECTED;
                                // Launch replay. Reset load flag so new replay waits
                                // until the client finishes loading the map (Fix #36).
                                replay_active_.store(false);     // signal old thread to stop
                                replay_map_loaded_.store(false); // reset load-ready for new session
                                if (replay_thread_.joinable()) replay_thread_.join();
                                replay_active_.store(true);
                                {
                                    std::string rkey = key;
                                    sockaddr_in raddr = client_addr;
                                    replay_thread_ = std::thread(
                                        [this, rkey, raddr]() { replay_loop(rkey, raddr); });
                                    spdlog::warn("[Replay] Replay thread launched (NMT_GameSpecific)");
                                }
                            }
                            // Don't send keepalive for unmatched NMTs — more types may follow
                        }
                    }
                    // Send a keepalive if no state advance happened
                    if (cs.nmt_state < 4 && !nmt_types.empty()) {
                        bool any_advanced = false;
                        for (int t : nmt_types) {
                            if ((t == 0 && cs.nmt_state >= 1) || (t == 5 && cs.nmt_state >= 2) ||
                                (t == 4 && cs.nmt_state >= 3) || (t == 9 && cs.nmt_state >= 4) ||
                                (t == 18 && cs.nmt_state >= 4))
                                any_advanced = true;
                        }
                        if (!any_advanced) send_keepalive(cs, client_addr, key);
                    }
                    // Fix #36: Detect map load completion.
                    // When the client sends non-NMT game data (bunch on ch > 0)
                    // AFTER NMT is complete, the map has finished loading and
                    // the game thread is running.  Signal the replay thread to
                    // start sending — up to this point it has been waiting so
                    // the OS receive buffer is empty and nothing gets dropped.
                    if (cs.nmt_state >= 4 && bunch_bits > 0 && nmt_types.empty()
                        && !replay_map_loaded_.load()) {
                        replay_map_loaded_.store(true);
                        spdlog::warn("[Replay] *** MAP LOADED: client sent game data on"
                                     " non-control ch — replay starting now");
                    }
                } else {
                    // ── NORMAL (non-replay) NMT STATE MACHINE ──────────
                    for (int cur_nmt : nmt_types) {
                        // Skip NMT processing during active bootstrap — client
                        // retransmits NMT but responding wastes seq numbers and
                        // injects keepalives that break the bootstrap data flow.
                        if (cs.bootstrap_active && cs.nmt_state >= 4) {
                            // NMT 23 = UnknownChannelType error from the client —
                            // NOT a retransmit.  It means a channel open we sent had
                            // a corrupt/unrecognised channel type (usually caused by
                            // a bad ChSeq patch at a false-positive bit offset).
                            if (cur_nmt == 23) {
                                spdlog::error("[GameServer] *** NMT_UnknownChannelType (23) from client! "
                                              "pkt seq={} acked={} — bootstrap ChSeq corruption likely ***",
                                              seq, acked_seq);
                            } else {
                                spdlog::info("[GameServer] Bootstrap active: client pkt seq={} acked={} "
                                             "(NMT retransmit {} ignored)",
                                             seq, acked_seq, cur_nmt);
                            }
                            continue;
                        }
                        if (cur_nmt == 0 && cs.nmt_state == 0) {
                            spdlog::info("[GameServer] ★ NMT_Hello received → sending NMT_Challenge");
                            send_nmt_challenge(cs, client_addr, key);
                            cs.nmt_state = 1;
                        } else if (cur_nmt == 5 && cs.nmt_state == 1) {
                            spdlog::info("[GameServer] ★ NMT_Login received → sending NMT_Welcome");
                            send_nmt_welcome(cs, client_addr, key);
                            cs.nmt_state = 2;
                        } else if (cur_nmt == 4 && cs.nmt_state >= 2) {
                            spdlog::info("[GameServer] ★ NMT_NetSpeed received");
                            send_keepalive(cs, client_addr, key);
                            cs.nmt_state = 3;
                        } else if ((cur_nmt == 9 || cur_nmt == 18) && cs.nmt_state >= 2) {
                            spdlog::info("[GameServer] ★★★ NMT_{}({}) — LAUNCHING BOOTSTRAP ★★★",
                                         cur_nmt == 9 ? "Join" : "GameSpecific", cur_nmt);
                            send_keepalive(cs, client_addr, key);
                            cs.nmt_state = 4;
                            cs.phase = ClientState::CONNECTED;
                            // Launch per-client bootstrap thread to send world state.
                            // cs.bootstrap_active prevents double-launch for the same
                            // client (e.g. NMT retransmits).  The class-level
                            // bootstrap_thread_ is joined first so we never have two
                            // simultaneous bootstrap threads (single-client emulator).
                            if (!cs.bootstrap_active) {
                                cs.bootstrap_active = true;
                                std::string bkey = key;
                                sockaddr_in baddr = client_addr;
                                if (bootstrap_thread_.joinable()) bootstrap_thread_.join();
                                bootstrap_thread_ = std::thread(
                                    [this, bkey, baddr]() { bootstrap_loop(bkey, baddr); });
                            }
                        }
                    }
                }

                // ── ClientAckGoodMove: acknowledge every ServerMove ─────────
                // Only respond once NMT negotiation is complete (nmt_state>=4).
                // Sending ACKs before that would inject unexpected game-channel
                // bunches that confuse the NMT state machine.
                if (!server_move_ts_list.empty()) {
                    spdlog::debug("[ACK-DBG] pending={} nmt_state={} cf_captured={}",
                                  server_move_ts_list.size(), cs.nmt_state,
                                  cs.custom_field_captured);
                }
                // REGRESSION GUARD (2026-04-20):
                // send_client_ack_good_move writes a bunch on ch=19 (same
                // channel as the replay's PlayerPawn).  With the current
                // 13-bit BDB framing it produces ContentBlockFail in the
                // client's PlayerPawn actor channel ("Bunch.IsError() == true
                // after SerializeObject. Actor: PlayerPawn_C_*") which
                // closes the connection.  The 2026-04-19 10:43 "WE MADE IT"
                // session (Sent: 29007 / 29010, world loaded, played like a
                // movie) did NOT call this path — it wasn't triggered, and
                // the client accepted the replay cleanly.  Disabling to
                // restore that baseline; re-enable once the ch=19 bunch
                // framing is validated byte-for-byte against the capture.
                constexpr bool kSendClientAckGoodMove = true;
                if (kSendClientAckGoodMove && cs.nmt_state >= 4) {
                    for (float sm_ts : server_move_ts_list) {
                        send_client_ack_good_move(cs, client_addr, key, sm_ts);
                    }
                }
            }
        }
    }
// [C | O | A + C.Y.B]

    /// The client closed its control channel: UE5's UNetConnection::Close
    /// sends a Close bunch on ch=0 and shuts the socket, so nothing more will
    /// arrive.  Dropping the phase below CONNECTED stops the replay loop, the
    /// post-replay keepalive loop and periodic keepalives (the same state the
    /// idle timeout uses).
    void handle_client_close(const std::string& key) {
        {
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(key);
            if (it == clients_.end() || it->second.phase < ClientState::HANDSHAKE_COMPLETE) return;
            it->second.phase = ClientState::AWAITING_INITIAL;
            it->second.pending_sulv_acks.clear();
            it->second.client_restart_pending = false;
        }
        spdlog::info("[GameServer] Client {} closed the connection (control-channel Close); "
                     "session dropped", key);
        if (live_world_) live_world_->on_client_disconnected(key);
    }

    /// Handles one C>S bunch whose header wire::walk_bunches already parsed
    /// (protocol/wire/packet_parser.h).  Returns the NMT message type (0-9)
    /// for a control-channel bunch, else -1 — which is NOT a parse failure.
    /// If sm_ts_out is non-null and the bunch is ServerMoveNoBase (ch=19, unreliable),
    /// the client's float TimeStamp is stored in *sm_ts_out.
    int try_parse_bunch(const uint8_t* data, size_t data_len,
                        const aoc::protocol::wire::ParsedBunch& hdr,
                        float* sm_ts_out = nullptr,
                        const std::string* client_key = nullptr,
                        const sockaddr_in* client_addr_for_reactive = nullptr) {
        int nmt_result = -1;
        const bool     b_ctrl           = hdr.is_control;
        const bool     b_open           = hdr.is_open;
        const bool     b_close          = hdr.is_close;
        const bool     b_reliable       = hdr.is_reliable;
        const bool     b_exports        = hdr.has_package_map_exports;
        const bool     b_guids          = hdr.has_must_map_guids;
        const bool     b_partial        = hdr.is_partial;
        const uint32_t ch_idx           = hdr.channel;
        const uint16_t ch_seq           = static_cast<uint16_t>(hdr.ch_sequence);
        const uint16_t bunch_data_bits  = static_cast<uint16_t>(hdr.bunch_data_bits);
        const size_t   bunch_data_start = hdr.data_start_bit;
        size_t b = bunch_data_start;

        spdlog::info("[GameServer]   Bunch: ctrl={} open={} close={} reliable={} paused={} ch={} exp={} guid={} partial={} chSeq={}",
                     b_ctrl, b_open, b_close, b_reliable, hdr.is_paused,
                     ch_idx, b_exports, b_guids, b_partial, ch_seq);
        if (!hdr.channel_name.empty())
            spdlog::info("[GameServer]   ChName: {}", hdr.channel_name);
        spdlog::info("[GameServer]   BunchDataBits={}", bunch_data_bits);

        if (bunch_data_bits > 0) {
            // First byte of bunch payload = NMT message type (for control channel)
            if (ch_idx == 0 && bunch_data_bits >= 8) {
                uint8_t nmt_type = static_cast<uint8_t>(ue5::read_bits(data, data_len, b, 8));
                nmt_result = nmt_type;
                spdlog::info("[GameServer]   >>> NMT Type: {} ({})",
                             nmt_type, aoc::net::nmt_name(nmt_type));

                // Dump NMT payload
                size_t nmt_rem = bunch_data_bits - 8;
                std::vector<uint8_t> nmt_bytes;
                if (nmt_rem > 0) {
                    size_t nmt_end = bunch_data_start + bunch_data_bits;
                    size_t save_b = b;
                    while (save_b + 8 <= nmt_end) {
                        nmt_bytes.push_back(static_cast<uint8_t>(ue5::read_bits(data, data_len, save_b, 8)));
                    }
                    spdlog::info("[GameServer]   NMT payload ({}B): {}",
                                 nmt_bytes.size(), ue5::hex_dump(nmt_bytes.data(), nmt_bytes.size(), 128));
                }

                // ── Session H.1: route this NMT to LiveWorld's dispatcher ──
                // The legacy reply path (state machine in the outer scope)
                // still runs; H.1 is additive observation.  When handlers
                // start generating S>C replies in H.2 we'll disable the
                // legacy reply for that opcode selectively.  `client_key`
                // is optional (nullptr when called outside recv context).
                if (client_key) {
                    maybe_dispatch_nmt_to_live_world(*client_key, nmt_type, nmt_bytes);
                }
            } else {
                // ── Game channel payload — extract and log bytes ──────
                // This is where ServerMove, chat RPCs, and all client
                // game data live.  Dumping it lets us reverse the RPC
                // format without a separate packet capture tool.
                std::vector<uint8_t> payload_bytes;
                size_t payload_end = bunch_data_start + bunch_data_bits;
                size_t tmp_b = bunch_data_start;
                while (tmp_b + 8 <= payload_end)
                    payload_bytes.push_back(
                        static_cast<uint8_t>(ue5::read_bits(data, data_len, tmp_b, 8)));
                // Partial last byte (if bunch_data_bits not byte-aligned)
                size_t leftover = (payload_end > tmp_b) ? (payload_end - tmp_b) : 0;
                if (leftover > 0)
                    payload_bytes.push_back(
                        static_cast<uint8_t>(ue5::read_bits(data, data_len, tmp_b, (int)leftover)));

                spdlog::info("[C>S] ch={} bits={} bytes={} open={} close={} reliable={} chSeq={} | {}",
                             ch_idx, bunch_data_bits, payload_bytes.size(),
                             b_open, b_close, b_reliable, ch_seq,
                             ue5::hex_dump(payload_bytes.data(), payload_bytes.size(), 128));
// (C | Y | B ~ CoA)

                // ── GUID-resolve sniffer ─────────────────────────────
                // When the client sees an S>C bunch referencing a
                // NetGUID it hasn't cached, UE's UPackageMapClient
                // replies with a C>S bunch carrying bGuids=true (and
                // usually bPartial=true for multi-segment path tables)
                // containing one or more unresolved package paths as
                // FStrings.  We can't decode the full Intrepid GUID
                // framing yet, but we CAN scan the payload for
                // printable-ASCII runs that look like UE asset paths
                // ("/Game/...", "/Script/...", "/Engine/...", etc.)
                // and dedupe them across the session.  The resulting
                // list is the exact set of references the client
                // cannot resolve — i.e., the shopping list for
                // whichever GUID-resolution path we build next.
                if (b_guids) {
                    // Per-process dedup set for UE asset paths observed
                    // in unresolved-GUID bunches — used only by the
                    // diagnostic logging below.  Bounded at 2048 entries
                    // so a long-lived multi-client server can't grow it
                    // without limit; at cap we stop inserting (keeps
                    // previous observations, drops new ones).  Not a
                    // per-client structure on purpose: duplicate paths
                    // across clients aren't interesting.
                    static constexpr size_t kSeenPathsCap = 2048;
                    static std::unordered_set<std::string> seen_paths;
                    static std::mutex seen_paths_mu;
                    // Sweep the payload for ASCII runs of length >=6
                    // starting with '/' and consisting of path chars.
                    auto is_path_char = [](uint8_t c) {
                        return (c >= 'A' && c <= 'Z')
                            || (c >= 'a' && c <= 'z')
                            || (c >= '0' && c <= '9')
                            || c == '/' || c == '_' || c == '.'
                            || c == '-' || c == '+' || c == ':';
                    };
                    size_t i = 0;
                    while (i < payload_bytes.size()) {
                        if (payload_bytes[i] != '/') { ++i; continue; }
                        size_t j = i;
                        while (j < payload_bytes.size()
                               && is_path_char(payload_bytes[j])) ++j;
                        size_t run_len = j - i;
                        if (run_len >= 6) {
                            std::string p(reinterpret_cast<const char*>(&payload_bytes[i]), run_len);
                            bool is_new = false;
                            {
                                std::lock_guard<std::mutex> lk(seen_paths_mu);
                                if (seen_paths.size() < kSeenPathsCap) {
                                    is_new = seen_paths.insert(p).second;
                                }
                            }
                            if (is_new) {
                                // Context bytes before the string help
                                // work out the GUID framing
                                // (packed int id, flags, FString len).
                                size_t ctx_start = (i >= 11) ? (i - 11) : 0;
                                std::vector<uint8_t> ctx(payload_bytes.begin() + ctx_start,
                                                         payload_bytes.begin() + i);
                                spdlog::warn("[GUID-SNIFF] NEW path (ch={} bits={}): "
                                             "'{}'  prefix_hex=[{}]  ({} unique so far)",
                                             ch_idx, bunch_data_bits, p,
                                             ue5::hex_dump(ctx.data(), ctx.size(), 32),
                                             (int)seen_paths.size());
                            } else {
                                spdlog::debug("[GUID-SNIFF] dup path (ch={}): '{}'",
                                              ch_idx, p);
                            }
                        }
                        i = (j > i) ? j : (i + 1);
                    }
                }
// [C O A :: Cyb]

                // ── Road A — Phase B.0d (2026-04-26) ────────────────
                // Recognizer: ServerNotifyLoadedWorld (post-streaming
                // RPC the client sends once World Partition Streaming
                // hits 100%).  Without our reply (ClientRestart) the
                // client gets stuck on the loading screen.
                //
                // RPC name "ServerNotifyLoadedWorld" was confirmed via
                // IDA decomp (sub_140FA5160 — UFunction registrar) and
                // its parameter "WorldPackageName" (FName).  The client
                // emits this as a reliable bunch on the PC channel
                // (ch=3) carrying the FName as an FString payload that
                // expands to "/Game/Levels/Verra_World_Master/Verra_World_Master".
                //
                // The FString is bit-packed inside the bunch and lands
                // at a non-byte-aligned offset, so when payload_bytes[]
                // reads it byte-aligned each character ends up shifted
                // 1 bit.  Hence the signature `5E 8E C2 DA CA` =
                // ('/'<<1) ('G'<<1) ('a'<<1) ('m'<<1) ('e'<<1).
                //
                // We also try the unshifted signature in case alignment
                // happens to land byte-aligned for any reason.
                //
                // This is OBSERVABILITY ONLY — it logs a loud line to
                // help us correlate timing with the WorldBootstrap
                // emission.  Reactive emission of ClientRestart will be
                // wired in a follow-up commit once we know exactly when
                // the client expects it.
                if (b_reliable && payload_bytes.size() >= 32) {
                    // Signatures for "/Game/Levels" — first 12 bytes is
                    // enough to be unique.  Each entry is 12 bytes.
                    static constexpr uint8_t kSigShifted[12] = {
                        0x5E, 0x8E, 0xC2, 0xDA, 0xCA,  // "/Game"
                        0x5E, 0x98, 0xCA, 0xEC, 0xCA, 0xD8, 0xE6  // "/Levels"
                    };
                    static constexpr uint8_t kSigDirect[12] = {
                        '/', 'G', 'a', 'm', 'e',
                        '/', 'L', 'e', 'v', 'e', 'l', 's'
                    };
                    bool found_shifted = false, found_direct = false;
                    size_t found_offset = 0;
                    if (payload_bytes.size() >= sizeof(kSigShifted)) {
                        for (size_t s = 0; s + sizeof(kSigShifted) <= payload_bytes.size(); ++s) {
                            if (std::memcmp(&payload_bytes[s], kSigShifted, sizeof(kSigShifted)) == 0) {
                                found_shifted = true; found_offset = s; break;
                            }
                            if (std::memcmp(&payload_bytes[s], kSigDirect, sizeof(kSigDirect)) == 0) {
                                found_direct = true; found_offset = s; break;
                            }
                        }
                    }
                    if (found_shifted || found_direct) {
                        // Hex-dump first 16 bytes (function ID + FName
                        // header — useful for further RE).
                        std::string hdr_hex;
                        for (size_t k = 0; k < std::min<size_t>(16, payload_bytes.size()); ++k) {
                            if (k) hdr_hex += ' ';
                            hdr_hex += spdlog::fmt_lib::format("{:02x}", payload_bytes[k]);
                        }
                        spdlog::warn("[NMT-DETECT] ★ ServerNotifyLoadedWorld DETECTED "
                                      "(ch={} chSeq={} bytes={} bits={} sig={} offset={}) "
                                      "hdr=[{}]",
                                      ch_idx, ch_seq, payload_bytes.size(), bunch_data_bits,
                                      found_shifted ? "shifted" : "direct", found_offset,
                                      hdr_hex);

                        // ── Road A — Phase B.0e (2026-04-26) ─────────────
                        // REACTIVE EMIT: re-send the captured ClientRestart
                        // bunch.  Per replay scan, captured pkt #134 is a
                        // ch=3 reliable bunch with bdb=128 bits = 16 bytes
                        // = exactly one FIntrepidNetGUID — the canonical
                        // shape of `ClientRestart(NewPawn)` per IDA decomp
                        // of sub_144412AB0 (APlayerController::ClientRestart
                        // calls ProcessRemoteFunction with one Pawn param).
                        //
                        // Idempotent: only fire once per client (suppress
                        // re-emission if the client retries the request
                        // before the previous emit has had time to apply).
                        // Tracked per-client-key in a static set under a
                        // mutex.
                        //
                        // Empirically the client sends ServerNotifyLoadedWorld
                        // ~4s into our bootstrap.  WorldBootstrap also emits
                        // pkt #134 around t=3.4s as part of its plan walk —
                        // but that's BEFORE the client requests it.  Re-
                        // emitting on detection ensures the client gets it
                        // exactly when expected.
                        // ── Road A — Phase B.0f ─────────────────────────
                        // Native ClientRestart emit using RE'd wire index.
                        // Hypothesis from binary RE of AOCClient-Win64-Shipping.exe:
                        //   ClientRestart wire index = 31 (RPC table pos 26 + 5 reserved)
                        // Validated against ServerNotifyLoadedWorld (table pos 62 + 5 = 67,
                        // matches captured byte 0x86 = (67<<1)|0).
                        //
                        // Pawn parameter: use the captured PC NetGUID 10341530
                        // as a placeholder.  For best results we'd use the
                        // actual captured Pawn NetGUID extracted from the
                        // spliced pkt#78, but that requires bit-level decode
                        // we haven't done yet.  If function index 31 is right,
                        // the client will at least process the bunch.
                        // ── Phase B.0p4 (2026-04-28 PM) ──────────────
                        // REACTIVE EMIT RE-ENABLED.
                        //
                        // Splice-only test (B.0p3) confirmed the
                        // captured pkt#134 splice does NOT trigger
                        // AcknowledgePossession on the client — the
                        // client receives the Pawn (it sends
                        // ServerMove on ch=3) but its
                        // AcknowledgedPawn pointer stays null,
                        // causing the "AAoCPlayerController - No
                        // valid pawn" loading-screen loop.
                        //
                        // The pkt#78 v3 fix (skip ch=0 bunch[1])
                        // means NetGUID 88 is now correctly
                        // registered on the client.  Once we've
                        // shipped pkt#78 (cs.pkt78_emitted=true) it's
                        // safe to fire a fresh native ClientRestart
                        // referencing NetGUID 88 — the client's
                        // PackageMap can resolve it and
                        // AcknowledgePossession() will fire,
                        // populating AcknowledgedPawn.
                        //
                        // Idempotent per-client via
                        // cs.reactive_clientrestart_sent so we don't
                        // re-emit on every SNLW retry.
                        if (client_key && client_addr_for_reactive) {
                            bool should_emit = false;
                            {
                                std::lock_guard<std::mutex> lk(client_mu_);
                                auto cit = clients_.find(*client_key);
                                if (cit != clients_.end()) {
                                    ClientState& cs = cit->second;
                                    if (cs.pkt78_emitted &&
                                        !cs.reactive_clientrestart_sent) {
                                        should_emit = true;
                                        cs.reactive_clientrestart_sent = true;
                                    } else if (!cs.pkt78_emitted) {
                                        spdlog::info("[NMT-DETECT] reactive "
                                            "ClientRestart deferred — pkt#78 "
                                            "not yet emitted (NetGUID 88 not "
                                            "registered on client)");
                                    } else {
                                        spdlog::debug("[NMT-DETECT] reactive "
                                            "ClientRestart already sent — "
                                            "ignoring SNLW retry");
                                    }
                                }
                            }
                            if (should_emit) {
                                // ── PM26 (2026-04-29) — DEFER to post-bootstrap ──
                                // Native CR now fires from on_world_bootstrap_complete
                                // where tracker is stable.  See PM26 notes there.
                                // Reset the flag so the post-bootstrap handler can fire.
                                {
                                    std::lock_guard<std::mutex> lk(client_mu_);
                                    auto cit = clients_.find(*client_key);
                                    if (cit != clients_.end())
                                        cit->second.reactive_clientrestart_sent = false;
                                }
                                spdlog::info("[NMT-DETECT] reactive emit DEFERRED "
                                    "(PM26) — native CR will fire post-bootstrap");
                                (void)client_addr_for_reactive;
                                goto pm26_skip_old_fire;
                            }
                            if (false) {
                                // ── PM25 (legacy, never reached after PM26) — kept for ref ──
                                //
                                // The pawn_idx fuzz in send_client_restart_native cycles
                                // through 6 NetGUID candidates from scan_actor_opens_pm24.py.
                                // PM24 single-fire test #1 (pkt62 ch=14) didn't trigger
                                // possession.  Rather than restart server 5 more times,
                                // fire all 6 in rapid succession from this single SNLW
                                // detection.  The right NetGUID will trigger
                                // ServerAcknowledgePossession; wrong ones are silently
                                // dropped by the client.
                                //
                                // KNOWN CONCERN — replay vs native chSeq conflict:
                                //   PM11 used kMinChSeqForCR=1000 to skip past PC chain
                                //   (chSeq 954-985).  But UE5 reliable channels need
                                //   STRICTLY contiguous chSeq.  If our 1000 jumps over
                                //   986-999, the client queues our bunch waiting for the
                                //   missing chSeq values that never arrive → never processed.
                                //
                                //   This may be why ALL native CR attempts in PM7-PM24
                                //   silently failed (queued, not processed).
                                //
                                //   The send_client_restart_native function still uses
                                //   the floor; for now we assume the client tolerates the
                                //   gap (or that we're past 1000 anyway by SNLW time).
                                //   If this fuzz fails, next step is to revise chSeq logic
                                //   to use exact tracker+1 with all-bunches walking.
                                //
                                // Fire all 6 candidates (loop in send_client_restart_native
                                // wraps via pawn_idx % count; each call advances pawn_idx).
                                spdlog::warn("[NMT-DETECT] → reactive emit ENABLED "
                                    "(PM25) — firing ALL 6 Pawn NetGUID candidates rapidly");
                                bool any_sent = false;
                                for (int i = 0; i < 6; ++i) {
                                    bool ok = send_client_restart_native(
                                        *client_key,
                                        *client_addr_for_reactive,
                                        /*pawn_obj_id=*/0,    // overridden by pawn_idx fuzz
                                        /*pawn_server_id=*/0,
                                        /*pawn_randomizer=*/0);
                                    if (ok) any_sent = true;
                                    // Small spdlog buffer flush between sends
                                }
                                if (!any_sent) {
                                    spdlog::warn("[NMT-DETECT] reactive "
                                        "ClientRestart all sends FAILED");
                                    std::lock_guard<std::mutex> lk(client_mu_);
                                    auto cit = clients_.find(*client_key);
                                    if (cit != clients_.end())
                                        cit->second.reactive_clientrestart_sent = false;
                                }
                            }
                            pm26_skip_old_fire:;
                        }
                    }
                }
// (C_Y_B ~ CoA)

                // ── Road A — Phase B.0h (2026-04-26) ────────────────
                // Recognizer: ServerCheckClientPossession.
                //
                // Per IDA decomp of sub_144412750
                // (APlayerController::ClientRestart_Implementation),
                // when ClientRestart is invoked with a NULL Pawn
                // parameter (because the NetGUID we sent doesn't
                // resolve client-side), the client immediately fires
                // `ServerCheckClientPossession` RPC back to the server.
                //
                // This gives us a UNIQUE OBSERVABLE: if our fuzz
                // burst includes a ClientRestart with the right
                // function index but wrong Pawn NetGUID, the client
                // sends ServerCheckClientPossession.  Detecting this
                // confirms our function index is correct — we just
                // need a valid Pawn NetGUID.
                //
                // The RPC is very small: ch=3 reliable, no parameters.
                // Per our table, ServerCheckClientPossession is at
                // RPC#58 (table pos 116 alphabetically, +5 reserved)
                // = wire index 63 → varint byte (63 << 1) = 0x7E.
                //
                // We detect it by signature: a small ch=3 reliable
                // bunch with the function index byte 0x7E early in
                // payload.  Conservative match: bunch_data_bits in
                // 8-50 range AND first byte == 0x7E.
                if (b_reliable && ch_idx == 3 && !b_partial && !b_exports
                    && bunch_data_bits >= 8 && bunch_data_bits <= 16384
                    && payload_bytes.size() >= 1) {
                    const uint8_t fb = payload_bytes[0];
                    // Decode varint byte → wire index → RPC name (per table)
                    // varint byte for value V: (V << 1) | 0 = V*2
                    // So wire_idx = fb >> 1
                    const uint32_t wire_idx = fb >> 1;
                    const char* rpc_name = nullptr;
                    bool is_success = false, is_fail = false;
                    bool is_request_init_char = false;  // PM28
                    switch (wire_idx) {
                    // PM107 (2026-05-04) — wire_idx=111 confirmed via IDA RE of
                    // AOC PlayerController function table at 0x14aa41c18 (16-byte
                    // stride): index 111 = ServerAcknowledgePossession.  Original
                    // guess at case 59 was unverified.  Empirical confirmation:
                    // captured logs show wire_idx=111 firing chSeq immediately after
                    // ClientRestart RPC (handle=45) reaches the client, which is the
                    // exact UE5 possession-handshake response.
                    case 111: rpc_name = "ServerAcknowledgePossession (★ SUCCESS — ClientRestart worked!)"; is_success = true; break;
                    // ── stale guesses kept for diagnostic context (likely wrong) ──
                    case 59: rpc_name = "?? wire_idx=59 (was: ServerAcknowledgePossession; superseded by 111)"; break;
                    case 60: rpc_name = "?? wire_idx=60 (was: ServerBlockPlayer guess)"; break;
                    case 61: rpc_name = "?? wire_idx=61 (was: ServerCamera guess)"; break;
                    case 62: rpc_name = "?? wire_idx=62 (was: ServerChangeName guess)"; break;
                    case 63: rpc_name = "?? wire_idx=63 (was: ServerCheckClientPossession guess)"; break;
                    case 64: rpc_name = "?? wire_idx=64 (was: ServerCheckClientPossessionReliable guess)"; break;
                    case 65: rpc_name = "?? wire_idx=65 (was: ServerExecRPC guess)"; break;
                    case 66: rpc_name = "?? wire_idx=66 (was: ServerMutePlayer guess)"; break;
                    case 67: rpc_name = "ServerNotifyLoadedWorld (empirical — chSeq pattern + 834-bit shape)"; break;
                    case 68: rpc_name = "?? wire_idx=68 (was: ServerPause guess)"; break;
                    case 69: rpc_name = "?? wire_idx=69 (was: ServerRestartPlayer guess)"; break;
                    // Neighbouring slots from the empirically-correct table at
                    // 0x14aa41c18 (1-indexed entry 112, 113, 114 for these):
                    case 112: rpc_name = "ServerBlockPlayer"; break;
                    case 113: rpc_name = "ServerCamera"; break;
                    case 110: rpc_name = "SendToConsole"; break;
                    // PM28 (2026-04-29) — recognize potential RequestInitializeCharacter
                    // candidates.  Exact wire_idx unknown; we fuzz a small range.
                    // Strong candidates based on alphabetical position + 5_reserved offset:
                    //   ServerRequestInitializeCharacter expected to be a server RPC
                    //   somewhere in 70-90 wire_idx range (alphabetical R-).
                    case 75: rpc_name = "?? wire_idx=75 (large RPC — possibly ServerRequestInitializeCharacter)"; is_request_init_char = true; break;
                    case 70: case 71: case 72: case 73: case 74:
                    case 76: case 77: case 78: case 79: case 80:
                    case 81: case 82: case 83: case 84: case 85:
                        rpc_name = "?? possible Server* character RPC"; is_request_init_char = (bunch_data_bits > 200); break;
                    default: break;
                    }
                    if (is_success) {
                        spdlog::warn("[NMT-DETECT] 🎉🎉🎉 {} (ch=3 chSeq={} bdb={} byte=0x{:02x})",
                                      rpc_name, ch_seq, bunch_data_bits, fb);
                    } else if (is_fail) {
                        spdlog::warn("[NMT-DETECT] ★★★ {} (ch=3 chSeq={} bdb={} byte=0x{:02x}) — "
                                      "DIAGNOSTIC: function index RIGHT for ClientRestart but "
                                      "Pawn NetGUID we sent doesn't resolve. Need captured Pawn GUID.",
                                      rpc_name, ch_seq, bunch_data_bits, fb);
                    } else if (rpc_name) {
                        spdlog::warn("[NMT-DETECT] {} (chSeq={} bdb={} byte=0x{:02x} wire_idx={})",
                                      rpc_name, ch_seq, bunch_data_bits, fb, wire_idx);
                    } else {
                        spdlog::info("[NMT-DETECT] unrecognized ch=3 RPC (chSeq={} bdb={} "
                                      "first_byte=0x{:02x} → wire_idx={}) — RPC not in table",
                                      ch_seq, bunch_data_bits, fb, wire_idx);
                    }
// [CoA :: C.Y.B]

                    // ── PM28 (2026-04-29) — extract FName CharacterName for response ──
                    if (is_request_init_char && client_key && client_addr_for_reactive
                        && payload_bytes.size() >= 8) {
                        // Try to extract the FName parameter that follows the dispatch byte.
                        // Wire format (CALV-style soft FName):
                        //   [8 bits] dispatch byte
                        //   [1 bit]  bHardcoded
                        //   if !bHardcoded:
                        //     [32 bits] FString length (incl NUL)
                        //     [N×8] ASCII chars
                        //     [8 bits] NUL
                        //     [32 bits] FName Number suffix
                        //   else:
                        //     [SIP] EName index
                        std::string extracted_name;
                        bool extracted = false;
                        // Read after dispatch byte at bit-offset 8 of payload
                        size_t pp = bunch_data_start + 8;
                        const size_t pend = bunch_data_start + bunch_data_bits;
                        if (pp + 1 <= pend) {
                            bool b_hard = ::ue5::read_bits(data, data_len, pp, 1) != 0;
                            if (!b_hard && pp + 32 <= pend) {
                                int32_t slen = static_cast<int32_t>(
                                    ::ue5::read_bits(data, data_len, pp, 32));
                                if (slen > 0 && slen <= 64 && pp + (size_t)slen * 8 <= pend) {
                                    for (int i = 0; i < slen - 1; ++i) {  // skip trailing NUL
                                        char c = static_cast<char>(
                                            ::ue5::read_bits(data, data_len, pp, 8));
                                        if (c >= 0x20 && c <= 0x7e) extracted_name += c;
                                    }
                                    if (!extracted_name.empty()) extracted = true;
                                }
                            }
                        }
                        if (extracted) {
                            spdlog::warn("[PM28] ★★ Recognized RequestInitializeCharacter — "
                                         "CharacterName='{}' (from C>S wire_idx={})",
                                         extracted_name, wire_idx);
                            // Fire ClientInitializeCharacter response with the extracted name
                            std::string keystr = *client_key;
                            sockaddr_in addr_copy = *client_addr_for_reactive;
                            std::string name_copy = extracted_name;
                            // Don't hold any locks — release will be done in caller path
                            send_client_initialize_character_native(
                                keystr, addr_copy, name_copy);
                        } else {
                            spdlog::info("[PM28] possible RequestInitializeCharacter "
                                         "(wire_idx={} bdb={}) but couldn't extract FName",
                                         wire_idx, bunch_data_bits);
                        }
                    }
                }

                // ServerUpdateLevelVisibility is decoded in the actor-RPC
                // dispatch below (PC fn_idx 92, protocol/wire/level_visibility.h).
                // The old "_Generated_/" ASCII scrape here matched bunches that
                // were only unreliable "synthetic channels" because of the C>S
                // PacketInfo misalignment; removed 2026-09-28.

                // Detect ServerMove: unreliable, non-ctrl, ServerMoveNoBase (fn_idx=44).
                // TimeStamp is an IEEE 754 float prefixed by 1 bit, followed by
                // InAccel (quantized 10.0) and ClientLoc (quantized 100.0).
                // ClientAckGoodMove reply echoes the float timestamp in 68-bit bunch.
                if (!b_reliable && !b_ctrl && (ch_idx == 19 || ch_idx == 9)) {
                    aoc::protocol::wire::FServerMoveNoBase sm_move;
                    if (aoc::protocol::wire::parse_server_move_no_base(
                            data, data_len, bunch_data_start, bunch_data_bits, sm_move)) {
                        if (sm_ts_out) {
                            *sm_ts_out = sm_move.timestamp;
                        }
                        spdlog::info("[C>S]   >> ServerMove parsed: ts={:.4f}s loc=({:.2f}, {:.2f}, {:.2f}) "
                                     "accel=({:.2f}, {:.2f}, {:.2f}) flags=0x{:02x} roll={}",
                                     sm_move.timestamp,
                                     sm_move.client_loc.x, sm_move.client_loc.y, sm_move.client_loc.z,
                                     sm_move.in_accel.x, sm_move.in_accel.y, sm_move.in_accel.z,
                                     sm_move.compressed_move_flags, sm_move.client_roll);
// (C O A | C:Y:B)

                        // Authoritative simulation update: push location into LiveWorld's ActorRegistry
                        if (client_key && live_world_) {
                            uint64_t pawn_netguid = 0;
                            if (auto* cs_session = live_world_->sessions().get(*client_key)) {
                                pawn_netguid = cs_session->pawn_netguid;
                            }
                            if (pawn_netguid == 0) {
                                pawn_netguid = 88; // Default captured pawn ObjectId in bootstrap replay
                            }
                            bool updated = live_world_->actors().set_location(
                                pawn_netguid,
                                {static_cast<float>(sm_move.client_loc.x),
                                 static_cast<float>(sm_move.client_loc.y),
                                 static_cast<float>(sm_move.client_loc.z)},
                                {static_cast<float>(sm_move.in_accel.x),
                                 static_cast<float>(sm_move.in_accel.y),
                                 static_cast<float>(sm_move.in_accel.z)});
                            if (updated) {
                                spdlog::debug("[LiveWorld] Updated pawn 0x{:x} location to ({:.2f}, {:.2f}, {:.2f})",
                                              pawn_netguid, sm_move.client_loc.x, sm_move.client_loc.y, sm_move.client_loc.z);
                            }

                            aoc::net::DispatchPacket dp;
                            dp.op         = aoc::net::DispatchOp::ACTOR_MOVEMENT;
                            dp.client_key = *client_key;
                            live_world_->on_packet(dp);
                        }
                    }
                }

                // Detect Actor / SubObject RPC: reliable, non-ctrl, channel 19, 9, or 3.
                if (b_reliable && !b_ctrl && (ch_idx == 19 || ch_idx == 9 || ch_idx == 3)) {
                    aoc::protocol::wire::FActorRpcBunch rpc_bunch;
                    if (aoc::protocol::wire::parse_actor_rpc_bunch(
                            data, data_len, bunch_data_start, bunch_data_bits, rpc_bunch, ch_idx)) {
                        std::string fn_name = rpc_bunch.function_name_hint();
                        if (rpc_bunch.fn_idx == 188) {
                            spdlog::info("[C>S-RPC] ch={} chSeq={} >> TRIGGER ABILITY: guid={} inst={} anim={} (target=0x{:x} Srv={})",
                                         ch_idx, ch_seq, rpc_bunch.trigger_params.guid_string(),
                                         rpc_bunch.trigger_params.client_instance_id,
                                         rpc_bunch.trigger_params.animation_index,
                                         rpc_bunch.target_guid, rpc_bunch.server_id);
                        } else if (rpc_bunch.fn_idx == 216) {
                            spdlog::info("[C>S-RPC] ch={} chSeq={} >> END ABILITY: server_inst={} (target=0x{:x} Srv={})",
                                         ch_idx, ch_seq, rpc_bunch.end_params.server_instance_id,
                                         rpc_bunch.target_guid, rpc_bunch.server_id);
                        } else if (rpc_bunch.fn_idx == 246) {
                            spdlog::info("[C>S-RPC] ch={} chSeq={} >> REQUEST HIT VERIFICATION: comp=\"{}\" ability=0x{:x} hit=0x{:x} (target=0x{:x} Srv={})",
                                         ch_idx, ch_seq, rpc_bunch.hit_params.target_component,
                                         rpc_bunch.hit_params.ability_guid,
                                         rpc_bunch.hit_params.hit_guid,
                                         rpc_bunch.target_guid, rpc_bunch.server_id);
                        } else if (rpc_bunch.fn_idx == 178) {
                            spdlog::info("[C>S-RPC] ch={} chSeq={} >> INTERACT WITH COMPONENT: guid=({} hi={} srv={} rnd=0x{:08x}) counter={} zone={}",
                                         ch_idx, ch_seq, rpc_bunch.interact_params.obj_lo,
                                         rpc_bunch.interact_params.obj_hi, rpc_bunch.interact_params.server_id,
                                         rpc_bunch.interact_params.randomizer, rpc_bunch.interact_params.counter,
                                         rpc_bunch.interact_params.zone_id);
                        } else if (rpc_bunch.fn_idx == 424) {
                            spdlog::info("[C>S-RPC] ch={} chSeq={} >> CHOOSE DIALOGUE OPTION: record_id=0x{:016x} ({})",
                                         ch_idx, ch_seq, rpc_bunch.dialogue_record_id,
                                         rpc_bunch.dialogue_record_id);
                        } else if (rpc_bunch.fn_idx == 834) {
                            spdlog::info("[C>S-RPC] ch={} chSeq={} >> SELECT DIALOGUE CHOICE: response=0x{:016x} ({})",
                                         ch_idx, ch_seq, rpc_bunch.dialogue_record_id,
                                         rpc_bunch.dialogue_record_id);
                        } else if (ch_idx == 3 && rpc_bunch.b_is_actor &&
                                   rpc_bunch.fn_idx == aoc::protocol::wire::kPcFieldIdx_ServerUpdateLevelVisibility) {
                            handle_server_update_level_visibility(
                                data, data_len, rpc_bunch,
                                bunch_data_start + bunch_data_bits, ch_idx, ch_seq,
                                client_key, client_addr_for_reactive);
                        } else if (ch_idx == 3 && rpc_bunch.b_is_actor &&
                                   (rpc_bunch.fn_idx == aoc::protocol::wire::kPcFieldIdx_ServerAcknowledgePossession ||
                                    rpc_bunch.fn_idx == aoc::protocol::wire::kPcFieldIdx_ServerCheckClientPossession ||
                                    rpc_bunch.fn_idx == aoc::protocol::wire::kPcFieldIdx_ServerCheckClientPossessionReliable)) {
                            handle_possession_rpc(
                                data, data_len, rpc_bunch,
                                bunch_data_start + bunch_data_bits, ch_idx, ch_seq,
                                client_key, client_addr_for_reactive);
                        } else {
                            spdlog::info("[C>S-RPC] ch={} chSeq={} is_actor={} target_guid=0x{:x} (Srv={}, Rnd=0x{:08x}) "
                                         "fn_idx={} ({}) cb_payload={} param_bits={}",
                                         ch_idx, ch_seq, rpc_bunch.b_is_actor, rpc_bunch.target_guid,
                                         rpc_bunch.server_id, rpc_bunch.randomizer,
                                         rpc_bunch.fn_idx, fn_name,
                                         rpc_bunch.cb_payload_bits,
                                         rpc_bunch.param_bit_count);
                        }

                        // The client batches RPCs: one bunch can hold several content
                        // blocks (replay-mode ServerAcknowledgePossession arrives with
                        // ServerRequestClientLoad and others).  Everything above handles
                        // the first block only; log the rest, and handle possession.
                        {
                            const size_t bunch_end = bunch_data_start + bunch_data_bits;
                            size_t next = rpc_bunch.cb_payload_start + rpc_bunch.cb_payload_bits;
                            for (int block = 1; block < 32 && next + 18 <= bunch_end; ++block) {
                                aoc::protocol::wire::FActorRpcBunch more;
                                if (!aoc::protocol::wire::parse_actor_rpc_bunch(
                                        data, data_len, next, bunch_end - next, more, ch_idx))
                                    break;
                                if (ch_idx == 3 && more.b_is_actor &&
                                    (more.fn_idx == aoc::protocol::wire::kPcFieldIdx_ServerAcknowledgePossession ||
                                     more.fn_idx == aoc::protocol::wire::kPcFieldIdx_ServerCheckClientPossession ||
                                     more.fn_idx == aoc::protocol::wire::kPcFieldIdx_ServerCheckClientPossessionReliable)) {
                                    handle_possession_rpc(data, data_len, more, bunch_end, ch_idx, ch_seq,
                                                          client_key, client_addr_for_reactive);
                                } else {
                                    spdlog::info("[C>S-RPC+] ch={} chSeq={} block {} is_actor={} fn_idx={} "
                                                 "cb_payload={}", ch_idx, ch_seq, block, more.b_is_actor,
                                                 more.fn_idx, more.cb_payload_bits);
                                }
                                next = more.cb_payload_start + more.cb_payload_bits;
                            }
                        }

                        if (client_key && live_world_) {
                            uint64_t target_guid = rpc_bunch.target_guid;
                            if (rpc_bunch.b_is_actor) {
                                if (auto* cs_session = live_world_->sessions().get(*client_key)) {
                                    target_guid = cs_session->pawn_netguid;
                                }
                                if (target_guid == 0) target_guid = 88;
                            }

                            aoc::net::DispatchPacket dp;
                            dp.op           = aoc::net::DispatchOp::ACTOR_RPC;
                            dp.client_key   = *client_key;
                            dp.u64_arg      = target_guid;
                            dp.fn_idx       = rpc_bunch.fn_idx;
                            dp.str_arg      = std::move(fn_name);
                            dp.payload      = std::move(rpc_bunch.param_bytes);
                            if (rpc_bunch.fn_idx == 188) {
                                dp.ability_guid = rpc_bunch.trigger_params.guid_string();
                                dp.instance_id  = rpc_bunch.trigger_params.client_instance_id;
                                dp.anim_index   = rpc_bunch.trigger_params.animation_index;
                            } else if (rpc_bunch.fn_idx == 216) {
                                dp.instance_id  = rpc_bunch.end_params.server_instance_id;
                            } else if (rpc_bunch.fn_idx == 246) {
                                dp.u64_arg       = rpc_bunch.hit_params.ability_guid;
                                dp.hit_guid      = rpc_bunch.hit_params.hit_guid;
                                dp.hit_component = rpc_bunch.hit_params.target_component;
                            }
                            live_world_->on_packet(dp);
// (C Y B + CoA)

                            // Hit verification -> ClientHitConfirmationsReliable on the PC
                            // (the ability component's ProxyMulticastHitConfirmation is
                            // server-to-server and the client rejects it).  Opt-in: the
                            // request decode above is heuristic, so GUIDs are placeholders.
                            if (rpc_bunch.fn_idx == 246 && client_addr_for_reactive &&
                                config_.send_hit_confirmations && !replay_active_.load()) {
                                std::lock_guard<std::mutex> lk(client_mu_);
                                auto it = clients_.find(*client_key);
                                if (it != clients_.end() && it->second.phase >= ClientState::CONNECTED) {
                                    ClientState& cs = it->second;
                                    // The client only knows the pawn the replay spawned;
                                    // LiveWorld's minted GUID never goes on the wire in
                                    // replay mode.
                                    uint64_t player_pawn_netguid = replay_pawn_guid().ObjectId;
                                    if (!replay_data_) {
                                        if (auto* cs_session = live_world_->sessions().get(*client_key))
                                            if (cs_session->pawn_netguid) player_pawn_netguid = cs_session->pawn_netguid;
                                    }

                                    uint64_t ability_rec = rpc_bunch.hit_params.full_ability_record;
                                    if (ability_rec == 0) {
                                        ability_rec = (rpc_bunch.hit_params.ability_guid << 16) | 0x01c5ULL;
                                    }
                                    uint64_t hit_rec = rpc_bunch.hit_params.full_hit_record;
                                    if (hit_rec == 0) {
                                        hit_rec = (rpc_bunch.hit_params.hit_guid << 16) | 0x01c6ULL;
                                    }

                                    aoc::protocol::wire::HitConfirmation hc;
                                    hc.inst_guid          = ++cs.hit_confirmations_sent;
                                    hc.record_guid        = hit_rec;
                                    hc.source_record_type = (ability_rec & 0xFFFF) ? (ability_rec & 0xFFFF) : 0x01c5;
                                    hc.source_record_guid = ability_rec;
                                    hc.instigator = { static_cast<uint32_t>(player_pawn_netguid & 0xFFFFFFFF),
                                                      static_cast<uint32_t>(player_pawn_netguid >> 32),
                                                      rpc_bunch.server_id, rpc_bunch.randomizer };
                                    hc.source = hc.instigator;
                                    hc.predicted_on_autonomous = true;
                                    (void)send_client_hit_confirmations(
                                        cs, *client_addr_for_reactive, *client_key, { hc });
                                }
                            } else if (rpc_bunch.fn_idx == 178 && client_addr_for_reactive) {
                                std::lock_guard<std::mutex> lk(client_mu_);
                                auto it = clients_.find(*client_key);
                                if (it != clients_.end() && it->second.phase >= ClientState::CONNECTED) {
                                    ClientState& cs = it->second;
                                    send_client_set_dialogue(
                                        cs, *client_addr_for_reactive, *client_key,
                                        rpc_bunch.interact_params.obj_lo,
                                        rpc_bunch.interact_params.obj_hi,
                                        rpc_bunch.interact_params.server_id,
                                        rpc_bunch.interact_params.randomizer);
                                }
                            }
                        }
                    }
                }
            }

        }
        return nmt_result;
    }

    /// Send an NMT message on the control channel inside a proper data packet.
    /// is_channel_open: if true, sends ctrl=0 (channel already open).
    ///                  if false, sends ctrl=1/open=1 to open the channel first.
    void send_nmt(ClientState& cs, const sockaddr_in& client_addr,
                  const std::string& key, uint8_t nmt_type,
                  const uint8_t* nmt_payload, size_t nmt_payload_bytes,
                  const char* desc, bool is_channel_open = false) {
        uint8_t buf[2048] = {};
        size_t off = 0;

        // -- Packet prefix (outer + notify + history + custom field) ----
        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);

        // PacketInfo (bHasPacketInfoPayload = 1 always for AoC)
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);  // bHasPacketInfoPayload
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10); // JitterClockTimeMS
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);  // bHasServerFrameTime

        bool writing_open = false;
        if (!is_channel_open) {
            // First NMT — open the control channel
            writing_open = true;
            ue5::write_bits(buf, sizeof(buf), off, 1, 1);  // bControl = 1
            ue5::write_bits(buf, sizeof(buf), off, 1, 1);  // bOpen = 1
            ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bClose = 0
        } else {
            // Channel already open — subsequent reliable NMT messages
            ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bControl = 0 (open/close not written)
        }
        // NMT uses C>S-compatible format: bIsReplicationPaused always present,
        // 10-bit ChSequence, bHardcoded+ChName, 13-bit BunchDataBits.
        // This is different from the S>C replication format (12-bit ChSeq, 14-bit dBits)
        // but it matches what the client expects for NMT on channel 0.
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bIsReplicationPaused = 0
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);  // bReliable = 1
        ue5::write_bits(buf, sizeof(buf), off, 0x00, 8); // ChIndex=0 (SerializeIntPacked)
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bHasPackageMapExports
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bHasMustBeMappedGUIDs
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bPartial
        ue5::write_bits(buf, sizeof(buf), off, cs.reliable_seq & 0x3FF, 10); // ChSequence (10-bit NMT)

        // ChName — UE5 reads ChName when (bReliable || bOpen) for NMT bunches.
        {
            ue5::write_bits(buf, sizeof(buf), off, 1, 1);    // bHardcoded = true
            ue5::write_bits(buf, sizeof(buf), off, 0xFF, 8);  // packed byte 1: 127 data + continue
            ue5::write_bits(buf, sizeof(buf), off, 0x02, 8);  // packed byte 2: 1 data + stop → idx=255
        }

        // BunchDataBits (13 bits for NMT format)
        uint16_t bunch_data_bits = static_cast<uint16_t>(8 + nmt_payload_bytes * 8);
        aoc::protocol::wire::write_bunch_data_bits(buf, sizeof(buf), off, bunch_data_bits);

        // \u2500\u2500 NMT payload \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
        ue5::write_bits(buf, sizeof(buf), off, nmt_type, 8);
        for (size_t i = 0; i < nmt_payload_bytes; ++i)
            ue5::write_bits(buf, sizeof(buf), off, nmt_payload[i], 8);

        // \u2500\u2500 Termination \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
        // Fix #15: extra sentinel bit for AoC data packets
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);

        uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;
        cs.reliable_seq++;

        int sent = send_to_client(buf, pkt_len, client_addr);
        spdlog::info("[GameServer] >> {} to {} ({}B seq={} ack={} chSeq={})",
                     desc, key, pkt_len, sent_seq, cs.in_ack_seq, cs.reliable_seq - 1);
        spdlog::info("[GameServer]    hex: {}", ue5::hex_dump(buf, pkt_len, 256));

        if (sent <= 0)
            spdlog::error("[GameServer]    Send {} FAILED", desc);
// -- C_Y_B . CoA --

        ProxyLogger::instance().log_udp("S>C", key, buf, pkt_len, desc);
    }

    /// Build FString payload bytes: int32(SaveNum) + chars + null
    static void fstring_to_payload(std::vector<uint8_t>& out, const std::string& s) {
        if (s.empty()) {
            out.push_back(0); out.push_back(0);
            out.push_back(0); out.push_back(0);
        } else {
            int32_t save_num = static_cast<int32_t>(s.size() + 1); // include null
            out.push_back(save_num & 0xFF);
            out.push_back((save_num >> 8) & 0xFF);
            out.push_back((save_num >> 16) & 0xFF);
            out.push_back((save_num >> 24) & 0xFF);
            for (char c : s) out.push_back(static_cast<uint8_t>(c));
            out.push_back(0); // null terminator
        }
    }

    /// Send NMT_Challenge(3) with a random challenge string.
    void send_nmt_challenge(ClientState& cs, const sockaddr_in& addr,
                            const std::string& key) {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        char ch_str[16];
        snprintf(ch_str, sizeof(ch_str), "%08X",
                 static_cast<uint32_t>(now & 0xFFFFFFFF));

        std::vector<uint8_t> payload;
        fstring_to_payload(payload, std::string(ch_str));

        // Challenge on channel 0 — already opened by client's NMT_Hello
        send_nmt(cs, addr, key, 3, payload.data(), payload.size(), "NMT_Challenge", true);
    }

    /// Send NMT_Welcome(1) with map, game mode, and redirect URL.
    void send_nmt_welcome(ClientState& cs, const sockaddr_in& addr,
                          const std::string& key) {
        std::vector<uint8_t> payload;
        // Map path — from captured real AoC server traffic
        fstring_to_payload(payload, "/Game/Levels/Verra_World_Master/Verra_World_Master");
        // GameMode class — from captured real AoC server traffic
        fstring_to_payload(payload, "/Game/GameBlueprints/AoCGameModeBaseBP.AoCGameModeBaseBP_C");
        // Redirect URL (empty)
        fstring_to_payload(payload, "");

        send_nmt(cs, addr, key, 1, payload.data(), payload.size(), "NMT_Welcome", true);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Bootstrap Sender — sends embedded world-state data after NMT_Join
    // ═══════════════════════════════════════════════════════════════════

    // ── ChSequence patching infrastructure ──────────────────────────
    // The bootstrap data contains ChSequence values from the original
    // capture session. The client initializes InReliable[ch] = ServerSeq % 4096
    // and expects ChSequence = InReliable[ch] + N.  We must rewrite all
    // ChSequence values by adding a delta: (current_base - orig_base) & 0xFFF.
    // ChSequence is 12 bits on the AoC wire (confirmed from bootstrap patcher).

    // ═══════════════════════════════════════════════════════════════════════
    // AoC S>C bunch header — canonical wire-format spec
    // ═══════════════════════════════════════════════════════════════════════
    //
    // This is the authoritative format description for the file.  All bunch
    // readers/writers below MUST match this layout.  Empirically validated
    // by replay_inspect against 56833 captured bunches.
    //
    //   bControl(1)
    //   if bControl:
    //       bCtrlOpen(1)
    //       bClose(1)
    //       if bClose: CloseReason(SerializeInt MAX=7, 1-3 adaptive bits)
    //   bIsReplicationPaused(1)                     ← always, outside ctrl
    //   bReliable(1)
    //   ChIndex(SerializeIntPacked — LSB=continuation, 7 data bits per byte)
    //   bHasPackageMapExports(1)                    ("bExports" in code)
    //   bHasMustBeMappedGUIDs(1)                    ("bGuids"   in code)
    //   bPartial(1)
    //   if bReliable: ChSequence(12 bits, ReadInt MAX=4096)
    //   if bPartial:  bPartialInitial(1)
    //                 bPartialCustomExportsFinal(1) ← AoC extension (3 flags)
    //                 bPartialFinal(1)
    //   if (bReliable || bCtrlOpen) && (!bPartial || bPartialInitial):
    //                 ChName = bHardcoded(1) + (SerializeIntPacked | FString)
    //   BunchDataBits(13 bits, ReadInt MAX=8192)
    //   <payload of BunchDataBits bits>
    //
    // KEY DIFFERENCES from standard UE5 C>S format:
    //   - NO bDataOpen/bDataClose for non-ctrl bunches — channel opens/closes
    //     always use bControl=1
    //   - ChName has a bHardcoded(1) prefix bit (UPackageMap::StaticSerializeName)
    //   - bPartial has 3 sub-fields (not 2) — AoC adds bPartialCustomExportsFinal
    //   - ChSequence is 12 bits (not stock UE5's 10)
    //
    // NOTE: a prior `has_control_bunch()` helper (dead code, never called)
    // was removed in the April 2026 cleanup pass.  If you need to scan for
    // ch=0 bunches again, use find_chseq_offsets() + check the ch_index
    // field on each ChSeqPatch entry.
    // ═══════════════════════════════════════════════════════════════════════


    struct ChSeqPatch {
        uint16_t bit_offset;      // bit offset of ChSequence within ALIGNED bunch data
        uint16_t ch_index;        // UE5 channel index
        bool     skip       = false; // true if coherence check flagged this as a false-positive
        bool     retransmit = false; // true if same raw chSeq as last accepted (ARQ retransmit)
    };

    /// Parse aligned bunch data to find all ChSequence bit positions for reliable bunches.
    /// Follows the canonical AoC S>C bunch header format documented above.
    static std::vector<ChSeqPatch> find_chseq_offsets(
        const uint8_t* data, size_t byte_len, size_t bunch_bits)
    {
        std::vector<ChSeqPatch> patches;
        size_t pos = 0;

        while (pos + 20 < bunch_bits) {
            size_t start = pos;

            bool bControl = ue5::read_bits(data, byte_len, pos, 1) != 0;
            bool bCtrlOpen = false;
            if (bControl) {
                bCtrlOpen = ue5::read_bits(data, byte_len, pos, 1) != 0;
                bool bClose = ue5::read_bits(data, byte_len, pos, 1) != 0;
                if (bClose) {
                    // CloseReason: SerializeInt(7), variable 1-3 bits
                    uint32_t v = 0, m = 1;
                    while ((v + m) < 7u && pos + 1 <= bunch_bits) {
                        if (ue5::read_bits(data, byte_len, pos, 1)) v |= m;
                        m <<= 1;
                    }
                }
            }
            ue5::read_bits(data, byte_len, pos, 1); // bIsReplicationPaused
// [C O A x Cyb]

            bool bReliable = ue5::read_bits(data, byte_len, pos, 1) != 0;

            // ChIndex (SerializeIntPacked)
            uint32_t ch_idx = 0;
            { int shift = 0;
              for (int i = 0; i < 5 && pos + 8 <= bunch_bits; ++i) {
                  uint32_t bv = static_cast<uint32_t>(ue5::read_bits(data, byte_len, pos, 8));
                  ch_idx |= ((bv >> 1) & 0x7F) << shift;
                  if (!(bv & 1)) break;
                  shift += 7;
              }
            }

            if (ch_idx > 32766u) break; // false positive → abort scan

            ue5::read_bits(data, byte_len, pos, 1); // bHasExports
            ue5::read_bits(data, byte_len, pos, 1); // bHasMustMap
            bool bPartial = ue5::read_bits(data, byte_len, pos, 1) != 0;

            // ChSequence — 10 bits via SerializeInt(MAX=1024), only for reliable bunches.
            // PM20 (2026-04-28): FIX — was 12 bits, but PM14 RE of sub_144230D50
            // line 1441 confirmed SerializeInt(MAX=1024) = 10 bits.
            // IMPORTANT: record the position TENTATIVELY and only commit it
            // after ChName parsing succeeds.  If ChName decodes as garbage
            // (invalid SaveNum, string too long, etc.) the parser has drifted
            // into a data payload — discard this position and stop scanning.
            size_t chseq_pos  = pos;
            bool   has_chseq  = bReliable && (pos + 10 <= bunch_bits);
            if (has_chseq)
                ue5::read_bits(data, byte_len, pos, 10); // skip ChSequence (commit later)

            if (bPartial) {
                ue5::read_bits(data, byte_len, pos, 1); // bPartialInitial
                ue5::read_bits(data, byte_len, pos, 1); // bPartialCustomExportsFinal (AoC ext)
                ue5::read_bits(data, byte_len, pos, 1); // bPartialFinal
            }
            // NO bDataOpen/bDataClose — those don't exist in S>C format!
            // ChName present when (bReliable || bCtrlOpen) — ALL reliable bunches!
            bool chname_ok = !(bReliable || bCtrlOpen); // trivially OK if no ChName field
            if (bReliable || bCtrlOpen) {
                if (pos + 1 <= bunch_bits) {
                    bool bHardcoded = ue5::read_bits(data, byte_len, pos, 1) != 0;
                    if (bHardcoded) {
                        // Hardcoded name: SerializeIntPacked index — always valid
                        chname_ok = true;
                        for (int i = 0; i < 5 && pos + 8 <= bunch_bits; ++i) {
                            uint32_t bv = static_cast<uint32_t>(
                                ue5::read_bits(data, byte_len, pos, 8));
                            if (!(bv & 1)) break;
                        }
                    } else {
                        // Soft (string) name: UE5 FString — int32 SaveNum + chars.
                        // SaveNum > 0: SaveNum ANSI chars (8 bits each).
                        // SaveNum < 0: -SaveNum UTF-16LE chars (16 bits each).
                        // An out-of-range SaveNum means we've drifted into a data
                        // payload — break WITHOUT recording the tentative ChSeq.
                        if (pos + 32 > bunch_bits) break; // not enough bits → drift
                        auto save_num = static_cast<int32_t>(
                            ue5::read_bits(data, byte_len, pos, 32));
                        bool bUnicode = save_num < 0;
                        int32_t char_count = bUnicode ? -save_num : save_num;
                        if (char_count < 0 || char_count > 256) break; // sanity → drift
                        size_t skip_bits = static_cast<size_t>(
                            bUnicode ? char_count * 16 : char_count * 8);
                        if (pos + skip_bits > bunch_bits) break;
                        pos += skip_bits;
                        chname_ok = true;
                    }
                }
            }

            // Commit the tentative ChSeq position only if ChName was valid AND
            // the position is not deep inside a data payload.
            //
            // Guard: if chseq_pos is in the last 5 % of the packet the parser
            // has almost certainly drifted into a payload (e.g. ch=12352@7474
            // in a 7665-bit packet = 97.5 %).  A real bunch starting that late
            // has <191 bits for its entire header + data, making it vanishingly
            // rare AND impossible to carry meaningful actor/channel-open data.
            // Patching those bits would corrupt the real payload that occupies
            // them, causing NMT_UnknownChannelType on the client.
            //
            // Threshold: chseq_pos * 20 > bunch_bits * 19  ⟺  pos > 95 %
            if (has_chseq && chname_ok) {
                if (chseq_pos * 20 > bunch_bits * 19) {
                    // Too close to end → stop scanning this packet
                    break;
                }
                patches.push_back({static_cast<uint16_t>(chseq_pos),
                                   static_cast<uint16_t>(ch_idx)});
            }

            if (pos + 13 > bunch_bits) break;
            uint32_t data_bits = static_cast<uint32_t>(ue5::read_bits(data, byte_len, pos, 13));
            if (data_bits > 8191) break;
            // If the data would overflow the packet the parser has drifted
            if (pos + data_bits > bunch_bits) break;
            pos += data_bits;

            if (pos <= start) break; // safety
        }

        return patches;
    }

    /// Build a packet from embedded bootstrap data.
    /// If patched_data is non-null, uses that aligned buffer instead of
    /// the raw bunch_data (for ChSequence-patched data with bit_shift=0).
    /// Writes: outer header + packed header + history + custom field +
    ///         PacketInfo + bunch data + sentinel + termination.
    /// Returns packet byte length, or 0 on failure.
    size_t build_bootstrap_packet(uint8_t* buf, size_t buf_cap,
                                  const bootstrap::BootstrapPacket& bpkt,
                                  ClientState& cs,
                                  const uint8_t* patched_data = nullptr) {
        if (!bpkt.bunch_data || bpkt.bunch_bits == 0 || buf_cap < 2048) return 0;

        std::memset(buf, 0, buf_cap);
        size_t off = 0;

        // ── Packet prefix (outer + notify + history + custom field) ───
        write_sc_packet_prefix(buf, buf_cap, off, cs,
                               /*handshake=*/false,
                               bpkt.hist_count);
// ~ Cyb - C O A ~

        // ── PacketInfo ─────────────────────────────────────────────────
        ue5::write_bits(buf, buf_cap, off, bpkt.has_pkt_info, 1);
        if (bpkt.has_pkt_info) {
            ue5::write_bits(buf, buf_cap, off, bpkt.jitter, 10);
        }
        ue5::write_bits(buf, buf_cap, off, bpkt.has_sft, 1);
        if (bpkt.has_sft) {
            ue5::write_bits(buf, buf_cap, off, bpkt.frame_time, 8);
        }

        // ── Bunch data ──────────────────────────────────────────────────
        // If patched_data is given, it's already aligned (bit_shift=0) with
        // corrected ChSequence values.  Otherwise use the raw embedded data.
        size_t src_bit, remaining, src_len;
        const uint8_t* src;
        if (patched_data) {
            src = patched_data;
            src_bit = 0;
            remaining = bpkt.bunch_bits;
            src_len = (bpkt.bunch_bits + 7) / 8;
        } else {
            src = bpkt.bunch_data;
            src_bit = bpkt.bit_shift;
            remaining = bpkt.bunch_bits;
            src_len = bpkt.bunch_bytes;
        }

        // Fast path: copy 8-bit chunks
        while (remaining >= 8 && (src_bit + 8) / 8 <= src_len) {
            // Read 8 bits from src at src_bit offset (LSB-first)
            uint8_t byte_val = 0;
            size_t sb = src_bit / 8;
            int    si = src_bit % 8;
            if (si == 0 && sb < src_len) {
                byte_val = src[sb];
            } else if (sb + 1 < src_len) {
                byte_val = static_cast<uint8_t>((src[sb] >> si) | (src[sb + 1] << (8 - si)));
            } else if (sb < src_len) {
                byte_val = static_cast<uint8_t>(src[sb] >> si);
            }
            ue5::write_bits(buf, buf_cap, off, byte_val, 8);
            src_bit += 8;
            remaining -= 8;
        }
        // Copy remaining bits
        for (size_t i = 0; i < remaining; ++i) {
            size_t sb = (src_bit + i) / 8;
            int    si = (src_bit + i) % 8;
            uint8_t bit = (sb < src_len) ? ((src[sb] >> si) & 1) : 0;
            ue5::write_bits(buf, buf_cap, off, bit, 1);
        }

        // ── Termination ────────────────────────────────────────────────
        // NO sentinel bit here — the captured bunch data already includes
        // all bunches; adding a sentinel=1 causes UE5 to try parsing a
        // phantom bunch from the termination padding → error → close.
        return ue5::add_termination(buf, buf_cap, off);
    }
// ~ C.O.A & C_Y_B ~

    // ── PlayerController Spawn ──────────────────────────────────────────────
    //
    // Sends a single reliable actor-channel Open bunch (S>C format) that asks
    // the client to spawn an AoCPlayerControllerBP_C actor.
    //
    // Wire layout (S>C actor channel — 12-bit ChSeq, 14-bit dBits):
    //
    //   Bunch header:
    //     bCtrl=1 bOpen=1 bClose=0  bIsRepPaused=0
    //     bReliable=1  ChIdx=PC_CHANNEL (SerializeIntPacked)
    //     bHasExports=1  bHasMustMap=0  bPartial=0
    //     ChSeq (12 bits)
    //     ChName = SerializeIntPacked(2)  (actor channel type "Actor")
    //     BunchDataBits (14 bits)
    //
    //   Payload (ReceiveNetGUIDBunch + SerializeNewActor):
    //     bRepLayout=0  NumGUIDs=1
    //       GUID packed64(4692) [static val=2346, PC archetype]
    //       flags=0x03 (has_path=1, no_load=1)
    //       outer packed64(0)  [null]
    //       FString ""         [SaveNum=0, empty name]
    //     actor_guid = packed64(ACTOR_DYN_WIRE)  [dynamic]
    //     archetype  = packed64(4692)              [static val=2346]
    //     bSerializeLocation=0  bSerializeRotation=0
    //     bSerializeScale=0     bSerializeVelocity=0
    //
    // The AoC client already has val=2346 in its local asset-registry cache
    // (confirmed: original server exported with flags=0x03, empty path).
    // The no_load flag tells the client not to try loading from disk.
    static constexpr uint16_t PC_CHANNEL        = 512;   // actor channel for the PC
    static constexpr uint64_t ARCH_STATIC_WIRE  = 4692;  // val=2346 static, wire=val*2=4692
    static constexpr uint64_t ACTOR_DYN_WIRE    = 3;     // val=1 dynamic,  wire=val*2+1=3

    void send_player_controller_spawn(ClientState& cs,
                                      const sockaddr_in& client_addr,
                                      const std::string& key) {
        uint8_t buf[512] = {};
        size_t  off = 0;

        // ── Packet prefix (outer + notify + history + custom field) ──────────
        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);

        // ── PacketInfo ────────────────────────────────────────────────────────
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);  // bHasPacketInfoPayload=1
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10); // JitterClockTimeMS
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);  // bHasServerFrameTime=0

        // ── Bunch header (S>C actor channel open) ─────────────────────────────
        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // bCtrl=1
        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // bOpen=1
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bClose=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bIsReplicationPaused=0
        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // bReliable=1

        // ChIdx (SerializeIntPacked) — PC_CHANNEL=512 → bytes 0x01 0x08
        ue5::write_sip(buf, sizeof(buf), off, PC_CHANNEL);

        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // bHasPackageMapExports=1
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bHasMustBeMappedGUIDs=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bPartial=0

        // ChSeq (12-bit) — first reliable on a fresh channel = in_reliable_base+1
        uint16_t pc_chseq = static_cast<uint16_t>((cs.in_reliable_base + 1) & 0xFFF);
        ue5::write_bits(buf, sizeof(buf), off, pc_chseq, 12);

        // ChName: bHardcoded(1)=1 prefix + SerializeIntPacked index
        // FName index 2 = "Actor" channel type → packed byte = (2<<1)|0 = 0x04
        ue5::write_bits(buf, sizeof(buf), off, 1,    1); // bHardcoded=1
        ue5::write_bits(buf, sizeof(buf), off, 0x04, 8); // nameIdx=2, no-continuation

        // BunchDataBits placeholder (13 bits) — patched after payload is known
        size_t bdb_off = off;
        ue5::write_bits(buf, sizeof(buf), off, 0, 13);

        // ── Payload start ──────────────────────────────────────────────────────
        size_t payload_start = off;

        // -- ReceiveNetGUIDBunch --
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bRepLayout=0
        ue5::write_bits(buf, sizeof(buf), off, 1, 32); // NumGUIDs=1 (int32 LE)

        // GUID entry: archetype, static val=2346, wire=4692
        ue5::write_sip(buf, sizeof(buf), off, ARCH_STATIC_WIRE);
        ue5::write_bits(buf, sizeof(buf), off, 0x03, 8); // flags: has_path=1 no_load=1
        ue5::write_bits(buf, sizeof(buf), off, 0x00, 8); // outer GUID = packed64(0)
        ue5::write_bits(buf, sizeof(buf), off, 0,    32); // FString name = "" (SaveNum=0)

        // -- SerializeNewActor --
        // Actor instance GUID (bare packed64, dynamic val=1 wire=3)
        ue5::write_sip(buf, sizeof(buf), off, ACTOR_DYN_WIRE);
        // Archetype GUID (bare packed64, static val=2346 wire=4692)
        ue5::write_sip(buf, sizeof(buf), off, ARCH_STATIC_WIRE);
        // Spawn-info flags: no location / rotation / scale / velocity
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bSerializeLocation=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bSerializeRotation=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bSerializeScale=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bSerializeVelocity=0

        // ── Patch BunchDataBits now that payload size is known ─────────────────
        size_t payload_bits = off - payload_start;
        ue5::patch_bits(buf, sizeof(buf), bdb_off,
                        static_cast<uint64_t>(payload_bits), 13);

        // ── Termination (extra sentinel + UE5 packet sentinel + pad) ──────────
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);

        uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        int sent = send_to_client(buf, pkt_len, client_addr);
        spdlog::warn("[PC-Spawn] >> PC Open ch={} chSeq={} archGUID={} payload={}b {}B seq={}",
                     PC_CHANNEL, pc_chseq, ARCH_STATIC_WIRE, payload_bits, pkt_len, sent_seq);
        spdlog::info("[PC-Spawn]    hex: {}", ue5::hex_dump(buf, pkt_len, 256));
        if (sent <= 0)
            spdlog::error("[PC-Spawn]    Send FAILED (sendto returned {})", sent);

        ProxyLogger::instance().log_udp("S>C", key, buf, pkt_len, "PC_Spawn");
    }

    /// Bootstrap loop: sends embedded world-state packets to bootstrap the client.
    /// Called after NMT_Join in emulation mode (no .rply file needed).
    void bootstrap_loop(const std::string& client_key, sockaddr_in client_addr) {
        spdlog::warn("[Bootstrap] ========================================");
        spdlog::warn("[Bootstrap]  BOOTSTRAP MODE STARTING (after NMT)");
        spdlog::warn("[Bootstrap]  Embedded packets: {}", bootstrap::BOOTSTRAP_PACKET_COUNT);

        // ── Phase 1: Pre-compute ChSequence patches with PER-CHANNEL deltas ──
        // AoC initializes InReliable[ch] = ServerSeq % 4096 for ALL channels.
        // Client expects first reliable ChSequence on any channel = InReliable + 1.
        // S>C ChSequence is 12-bit (MAX_CHSEQUENCE = 4096).
        // Each channel in the captured data has a different starting ChSeq,
        // so we compute an independent delta for each channel to map its first
        // reliable ChSeq → (current_base + 1).

        struct PktPatchInfo {
            std::vector<uint8_t> aligned;   // aligned bunch data (bit_shift=0)
            std::vector<ChSeqPatch> patches; // ChSequence positions
        };
        std::vector<PktPatchInfo> pkt_info(bootstrap::BOOTSTRAP_PACKET_COUNT);

        // Track first ChSequence seen per channel
        std::unordered_map<uint16_t, uint16_t> first_chseq_per_ch; // ch → first ChSeq
        int total_patches = 0;
// C-O-A - Cyb

        for (size_t i = 0; i < bootstrap::BOOTSTRAP_PACKET_COUNT; ++i) {
            const auto& bpkt = bootstrap::PACKETS[i];
            if (!bpkt.bunch_data || bpkt.bunch_bits == 0) continue;

            auto aligned = bootstrap::extract_bunch_bits(bpkt);
            auto patches = find_chseq_offsets(aligned.data(), aligned.size(), bpkt.bunch_bits);

            // Record first ChSequence for each channel
            for (const auto& patch : patches) {
                if (first_chseq_per_ch.find(patch.ch_index) == first_chseq_per_ch.end()) {
                    uint16_t chseq = static_cast<uint16_t>(
                        ue5::read_bits_at(aligned.data(), aligned.size(),
                                         patch.bit_offset, 12));
                    first_chseq_per_ch[patch.ch_index] = chseq;
                }
            }

            total_patches += static_cast<int>(patches.size());
            pkt_info[i].aligned = std::move(aligned);
            pkt_info[i].patches = std::move(patches);
        }

        // Get current session's InReliable base
        uint16_t current_base = 0;
        {
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(client_key);
            if (it == clients_.end()) return;
            auto& cs = it->second;
            current_base = cs.in_reliable_base;
            spdlog::warn("[Bootstrap]  Session state: out_seq={} in_ack_seq={} out_ack_seq={}",
                         cs.out_seq, cs.in_ack_seq, cs.out_ack_seq);
            spdlog::warn("[Bootstrap]  reliable_seq={} in_reliable_base={} custom_field={}",
                         cs.reliable_seq, cs.in_reliable_base,
                         cs.custom_field_captured ? "yes" : "NO");
        }

        // Compute per-channel deltas: target = current_base + 1 for first reliable bunch
        // delta[ch] = (current_base + 1 - first_chseq[ch]) & 0xFFF (12-bit)
        std::unordered_map<uint16_t, uint16_t> delta_per_ch;
        for (const auto& [ch, first_seq] : first_chseq_per_ch) {
            uint16_t target = (current_base + 1) & 0xFFF;
            uint16_t delta = (target - first_seq) & 0xFFF;
            delta_per_ch[ch] = delta;
        }

        spdlog::warn("[Bootstrap]  Current InReliable base: {} → target first ChSeq: {}",
                     current_base, (current_base + 1) & 0xFFF);
        spdlog::warn("[Bootstrap]  Per-channel deltas: {} channels, {} total patches",
                     delta_per_ch.size(), total_patches);
        // Log first few channel deltas
        {
            int logged = 0;
            for (const auto& [ch, delta] : delta_per_ch) {
                if (logged >= 10) { spdlog::info("[Bootstrap]    ... and {} more channels", delta_per_ch.size() - 10); break; }
                spdlog::info("[Bootstrap]    ch={}: first_chseq={} → delta={}",
                             ch, first_chseq_per_ch[ch], delta);
                ++logged;
            }
        }
// C.O.A + Cyb

        // ── Phase 1b: Forward-only coherence filter (retransmit-aware) ─────────
        // The bootstrap capture is NON-CONTIGUOUS: intermediate packets from the
        // original session are absent, so per-channel chSeq values will jump
        // forward (e.g. 1342 → 1348 when pkt#3 to pkt#31 skips 5 intermediates).
        // Those forward gaps are NORMAL and must be accepted.
        //
        // Some channels (ch=125, ch=117, etc.) had their open bunch retransmitted
        // multiple times due to ARQ before the ACK arrived.  A retransmit carries
        // the SAME raw chSeq as the original.  After the original is accepted,
        // expected_next[ch] = raw+1, so the retransmit produces gap = 4095
        // (one behind expected) — indistinguishable from a false positive by the
        // gap check alone.  We distinguish them using last_raw[ch]: if the
        // suspicious value equals the last ACCEPTED raw chSeq, it is a retransmit
        // and must be renumbered with the same assigned chSeq (so the client's
        // duplicate-detection discards it cleanly instead of seeing a stale value).
        //
        // Rule:
        //   gap ≤ MAX_JUMP                     → ACCEPT (forward / exact)
        //   gap > MAX_JUMP && raw == last_raw  → RETRANSMIT (re-send same chSeq)
        //   gap > MAX_JUMP && raw != last_raw  → FALSE-POS (skip / corrupt)
        //
        // First occurrence of any channel is always accepted as the seed.
        {
            constexpr uint16_t MAX_JUMP = 512;
            std::unordered_map<uint16_t, uint16_t> expected_next; // ch → next raw chSeq expected
            std::unordered_map<uint16_t, uint16_t> last_raw;      // ch → last accepted raw chSeq
            int accepted = 0, retransmits = 0, rejected = 0;
            for (size_t i = 0; i < bootstrap::BOOTSTRAP_PACKET_COUNT; ++i) {
                auto& pi = pkt_info[i];
                for (auto& patch : pi.patches) {
                    uint16_t chseq = static_cast<uint16_t>(
                        ue5::read_bits_at(pi.aligned.data(), pi.aligned.size(),
                                         patch.bit_offset, 12));
                    auto eit = expected_next.find(patch.ch_index);
                    if (eit == expected_next.end()) {
                        // First occurrence → accept as seed unconditionally
                        expected_next[patch.ch_index] = (chseq + 1) & 0xFFF;
                        last_raw[patch.ch_index]      = chseq;
                        ++accepted;
                    } else {
                        // Forward distance in 12-bit modular space:
                        //   gap = 0 → exact match (no missing pkts)
                        //   gap ∈ (0, MAX_JUMP] → forward gap (capture skip)
                        //   gap > MAX_JUMP → backward — retransmit or false-positive
                        uint16_t gap = (chseq - eit->second) & 0xFFF;
                        if (gap <= MAX_JUMP) {
                            eit->second              = (chseq + 1) & 0xFFF;
                            last_raw[patch.ch_index] = chseq;
                            ++accepted;
                        } else {
                            // Backward gap — distinguish retransmit from false-positive
                            auto lr_it = last_raw.find(patch.ch_index);
                            if (lr_it != last_raw.end() && chseq == lr_it->second) {
                                // Same raw chSeq as last accepted → ARQ retransmit
                                patch.retransmit = true;
                                ++retransmits;
                                spdlog::info("[Bootstrap] RETRANSMIT ChSeq: pkt#{} ch={} "
                                             "chseq={} (matches last_raw) bit_off={}",
                                             i + 1, patch.ch_index, chseq, patch.bit_offset);
                                // Do NOT advance expected_next or last_raw
                            } else {
                                // Genuine false positive
                                patch.skip = true;
                                ++rejected;
                                if (rejected <= 20)
                                    spdlog::error("[Bootstrap] FALSE-POS ChSeq: pkt#{} ch={} "
                                                  "got={} gap={} expected={} bit_off={} → SKIP",
                                                  i + 1, patch.ch_index, chseq, gap,
                                                  eit->second, patch.bit_offset);
                                // Do NOT advance expectation — real sequence continues
                            }
                        }
                    }
                }
            }
            spdlog::warn("[Bootstrap]  ChSeq filter: {} accepted, {} retransmits, {} rejected (false-pos)",
                         accepted, retransmits, rejected);
        }

        // ── Phase 2: Renumber ChSequence values (sequential, gapless) ────────
        // Instead of adding a per-channel delta to the raw captured value, assign
        // a FRESH sequential counter to each accepted position.  This handles both
        // the base-offset difference AND the non-contiguous gaps in the capture:
        // the client receives a clean gapless sequence on every channel.
        //
        // Each channel's counter starts at (current_base + 1) mod 4096, matching
        // the UE5 convention that InReliable[ch] is initialised from the packet
        // sequence counter at connection time.
        //
        // Retransmits: patch with the SAME chSeq that was assigned to the original
        // send.  The client's duplicate detection (InReliable[ch] already advanced)
        // will discard the duplicate cleanly.  This is correct UE5 behaviour: a
        // retransmitted reliable bunch must carry the original sequence number.
        {
            std::unordered_map<uint16_t, uint16_t> ch_next;       // ch → next chSeq to assign
            std::unordered_map<uint16_t, uint16_t> last_assigned; // ch → last chSeq assigned
            int renumbered = 0, retransmits = 0, skipped = 0;
            for (size_t i = 0; i < bootstrap::BOOTSTRAP_PACKET_COUNT; ++i) {
                auto& pi = pkt_info[i];
                if (pi.patches.empty()) continue;
                for (const auto& patch : pi.patches) {
                    if (patch.skip) { ++skipped; continue; }

                    // Initialise counter on first use for this channel
                    auto it = ch_next.find(patch.ch_index);
                    if (it == ch_next.end()) {
                        ch_next[patch.ch_index] = (current_base + 1) & 0xFFF;
                        it = ch_next.find(patch.ch_index);
                    }
// (C.O.A : Cyb)

                    uint16_t new_chseq;
                    if (patch.retransmit) {
                        // Re-use the chSeq assigned to the original send
                        auto la_it = last_assigned.find(patch.ch_index);
                        if (la_it != last_assigned.end()) {
                            new_chseq = la_it->second;
                            ++retransmits;
                        } else {
                            // Retransmit before original (shouldn't happen) — treat as new
                            new_chseq = it->second;
                            it->second = (new_chseq + 1) & 0xFFF;
                            last_assigned[patch.ch_index] = new_chseq;
                            ++renumbered;
                        }
                    } else {
                        new_chseq = it->second;
                        it->second = (new_chseq + 1) & 0xFFF;
                        last_assigned[patch.ch_index] = new_chseq;
                        ++renumbered;
                    }

                    ue5::patch_bits(pi.aligned.data(), pi.aligned.size(),
                                   patch.bit_offset, new_chseq, 12);
                }
            }
            spdlog::info("[Bootstrap]  Renumbered {} ChSequence values ({} retransmits patched, {} skipped/false-pos)",
                         renumbered, retransmits, skipped);
        }

        spdlog::warn("[Bootstrap]  Adaptive pacing: 1ms/20ms/100ms/500ms");
        spdlog::warn("[Bootstrap] ========================================");

        // ── Phase 3: Send packets ──────────────────────────────────────
        constexpr size_t BUF_CAP = 16384;
        std::vector<uint8_t> buf_vec(BUF_CAP, 0);
        uint8_t* buf = buf_vec.data();

        size_t sent_count = 0;
        size_t error_count = 0;
        size_t skip_count = 0;
        uint16_t prev_client_ack = 0;   // stall detection (per-loop locals, not static)
        int stall_count = 0;

        for (size_t i = 0; i < bootstrap::BOOTSTRAP_PACKET_COUNT && running_; ++i) {
            const auto& bpkt = bootstrap::PACKETS[i];

            // Skip empty packets (no bunch data)
            if (!bpkt.bunch_data || bpkt.bunch_bits == 0) {
                ++skip_count;
                continue;
            }

            // (ch=0 filtering is now done at generation time by gen_bootstrap_data.py)

            // ── Adaptive pacing ────────────────────────────────────────
            {
                int ahead = 0;
                {
                    std::lock_guard<std::mutex> lk(client_mu_);
                    auto it = clients_.find(client_key);
                    if (it == clients_.end()) break;
                    auto& cs = it->second;
                    if (cs.phase < ClientState::CONNECTED) {
                        spdlog::warn("[Bootstrap] Client disconnected, stopping");
                        break;
                    }
                    ahead = static_cast<int>((cs.out_seq - cs.out_ack_seq) & ClientState::SEQ_MASK);
                    if (ahead > 8192) ahead -= 16384;
                    if (ahead > 500) {
                        spdlog::warn("[Bootstrap] Client unresponsive (ahead={}), stopping", ahead);
                        break;
                    }
                }

                // Conservative pacing to prevent client UDP buffer overflow.
                // Windows default UDP rcvbuf ~8KB = ~8 packets of 978B.
                // Even on localhost, aggressive sending overflows the client.
                int delay_ms = 5;  // base: 200 pkt/s max
                if (ahead >= 40) delay_ms = 500;   // ~2 pkt/s
                else if (ahead >= 30) delay_ms = 200; // ~5 pkt/s
                else if (ahead >= 20) delay_ms = 50;  // ~20 pkt/s
                else if (ahead >= 10) delay_ms = 20;  // ~50 pkt/s

                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }

            // ── Build and send packet ──────────────────────────────────
            std::memset(buf, 0, BUF_CAP);
            size_t pkt_len = 0;
            uint16_t pkt_seq = 0, pkt_ack = 0, client_ack = 0;

            {
                std::lock_guard<std::mutex> lk(client_mu_);
                auto it = clients_.find(client_key);
                if (it == clients_.end() || it->second.phase < ClientState::CONNECTED) {
                    spdlog::error("[Bootstrap] Client {} gone, stopping", client_key);
                    break;
                }
                auto& cs = it->second;
                pkt_seq = cs.out_seq;
                pkt_ack = cs.in_ack_seq;
                client_ack = cs.out_ack_seq;
                // Use patched aligned data if available
                const uint8_t* pdata = pkt_info[i].aligned.empty()
                    ? nullptr : pkt_info[i].aligned.data();
                pkt_len = build_bootstrap_packet(buf, BUF_CAP, bpkt, cs, pdata);
                if (pkt_len > 0) {
                    cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;
                } else {
                    ++skip_count;
                }
            }

            if (pkt_len > 0) {
                int sent = send_to_client(buf, pkt_len, client_addr);
                if (sent > 0) {
                    ++sent_count;
                    int ahead = static_cast<int>((pkt_seq - client_ack) & 0x3FFF);
                    if (ahead > 8192) ahead -= 16384;

                    // ── Bunch summary for diagnostics (skip=false only) ──
                    std::string bunch_summary;
                    {
                        const auto& pi = pkt_info[i];
                        if (!pi.patches.empty() || !pi.aligned.empty()) {
                            // Skip false-positive positions; annotate retransmits with 'r'
                            // Also show bit_off so we can verify the patch lands at a
                            // plausible ChSeq position (helps spot false-positive seeds).
                            for (const auto& p : pi.patches) {
                                if (p.skip) continue;
                                uint16_t chseq = static_cast<uint16_t>(
                                    ue5::read_bits_at(pi.aligned.data(), pi.aligned.size(),
                                                     p.bit_offset, 12));
                                if (!bunch_summary.empty()) bunch_summary += ", ";
                                bunch_summary += "ch=" + std::to_string(p.ch_index)
                                    + "@" + std::to_string(p.bit_offset)
                                    + ":" + std::to_string(chseq);
                                if (p.retransmit) bunch_summary += "r"; // retransmit marker
                            }
                        }
                        if (bunch_summary.empty()) bunch_summary = "(no-rel)";
                    }
// C_Y_B / C.O.A

                    // Log ALL bootstrap packets (full visibility)
                    spdlog::info("[Bootstrap] >> #{} {}B seq={} ack={} ahead={} [{}]",
                                 i + 1, pkt_len, pkt_seq, pkt_ack, ahead, bunch_summary);

                    // Detect client ack stall (non-static: per-bootstrap-loop locals)
                    if (sent_count > 1 && client_ack == prev_client_ack) {
                        ++stall_count;
                        if (stall_count == 5)
                            spdlog::warn("[Bootstrap] ⚠ Client ack STALLED at {} for {} packets",
                                         client_ack, stall_count);
                        else if (stall_count == 20)
                            spdlog::warn("[Bootstrap] ⚠ Client ack STALLED at {} for {} packets — likely disconnected",
                                         client_ack, stall_count);
                    } else {
                        if (stall_count > 3)
                            spdlog::info("[Bootstrap] Client ack resumed after {} stalled packets (now={})",
                                         stall_count, client_ack);
                        stall_count = 0;
                    }
                    prev_client_ack = client_ack;

                    // Log first 3 packets hex header for verification
                    if (sent_count <= 3)
                        spdlog::info("[Bootstrap]    first16: {}", ue5::hex_dump(buf, std::min(pkt_len, size_t(16)), 16));
                } else {
                    ++error_count;
                    if (error_count <= 5)
                        spdlog::warn("[Bootstrap] Send failed at #{}", i + 1);
                }
            }
        }

        spdlog::warn("[Bootstrap] ========================================");
        spdlog::warn("[Bootstrap]  BOOTSTRAP COMPLETE");
        spdlog::warn("[Bootstrap]  Sent: {} (skipped {}, errors {})",
                     sent_count, skip_count, error_count);
        spdlog::warn("[Bootstrap] ========================================");

        // Reset last_activity so the idle timer starts fresh from now.
        // bootstrap_active stays TRUE here — the timeout check uses it to
        // apply the 300s limit instead of 30s.  We clear it only AFTER the
        // post-bootstrap keepalive loop so the timeout never reverts to 30s
        // while the client is still loading the world.
        {
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(client_key);
            if (it != clients_.end())
                it->second.last_activity = std::chrono::steady_clock::now();
        }

        // Spawn the PlayerController actor so the client enters the game world.
        {
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(client_key);
            if (it != clients_.end() && it->second.phase >= ClientState::CONNECTED) {
                spdlog::warn("[Bootstrap]  Sending PlayerController spawn...");
                send_player_controller_spawn(it->second, client_addr, client_key);
            }
        }

        // Keep connection alive after bootstrap.
        // bootstrap_active remains true throughout so the 300s idle timeout
        // stays in effect while the client finishes loading the world.
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(client_key);
            if (it == clients_.end() || it->second.phase < ClientState::CONNECTED) break;
            send_keepalive(it->second, client_addr, client_key);
        }

        // Clear the per-client flag NOW (after keepalive loop) so a reconnect
        // can trigger a fresh bootstrap, and so the normal 30s idle timeout
        // resumes only after the client has fully entered the world.
        {
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(client_key);
            if (it != clients_.end()) it->second.bootstrap_active = false;
        }
    }

    // ── Packet Replay Engine ────────────────────────────────────────────

    // NOTE: an audit_sc_bunches helper was removed in the 2026-04 cleanup.
    // If you need a per-packet S>C bunch audit, reuse replay_inspect.cpp's
    // parse_sc_bunch as the canonical reference — it is validated against
    // 56 833 captured bunches with zero drift.

// CoA.C.Y.B

    /// Build a replay packet from scratch: outer header, packed header,
    /// history, PacketInfo, custom field (from CURRENT session), and
    /// bunch data (verbatim from original capture).  This ensures the
    /// custom field matches the new session's value, not the original's.
    // Verbose bunch logging — parses all bunches embedded in a replay
    // packet and prints each one in human-readable form.  Guarded by
    // config_.verbose_bunches and a per-session cap to avoid drowning
    // the log on a full replay.  Uses the shared aoc::parse_sc_bunch
    // parser from net/sc_bunch_parser.h.
    //
    // PHASE 2c — foundation for live-traffic visibility.
    void log_replay_packet_bunches(const ReplayPacketInfo& rpkt,
                                   uint16_t sent_seq) {
        if (!config_.verbose_bunches) return;
        if (config_.verbose_bunch_limit > 0
            && verbose_logged_bunches_ >= config_.verbose_bunch_limit) return;
        if (rpkt.bunch_bits == 0) return;
        // Skip trivially-small packets (UE5 sentinel / keepalive with no real
        // bunch content — can't possibly contain a 20-bit bunch header).
        if (rpkt.bunch_bits < 20) return;
        // Honor --verbose-bunch-start: skip the Nth first replay packets
        // before we begin logging, to sample mid-replay traffic.
        verbose_seen_packets_++;
        if (verbose_seen_packets_ <= config_.verbose_bunch_start) return;

        // Aggregate session stats (flushed at shutdown — see close helper).
        verbose_packet_count_++;

        // Lazy-open the dedicated file sink on first use.
        if (!config_.verbose_bunch_log.empty() && !verbose_bunch_log_stream_) {
            verbose_bunch_log_stream_ = std::make_unique<std::ofstream>(
                config_.verbose_bunch_log, std::ios::out | std::ios::trunc);
            if (*verbose_bunch_log_stream_) {
                *verbose_bunch_log_stream_
                    << "# [SEND] dump — one line per outgoing bunch.\n"
                    << "# Columns: pkt_seq | bunch_idx | ch | kind | bits | "
                    << "rel | partIIF | exp | gui | ctrl | chSeq | name\n";
                verbose_bunch_log_stream_->flush();
                spdlog::info("[SEND] verbose bunch output -> '{}'",
                             config_.verbose_bunch_log);
            } else {
                spdlog::warn("[SEND] failed to open '{}' — falling back to emu log",
                             config_.verbose_bunch_log);
                verbose_bunch_log_stream_.reset();
            }
        }

        auto emit = [&](const char* line) {
            if (verbose_bunch_log_stream_) {
                *verbose_bunch_log_stream_ << line << '\n';
                verbose_bunch_log_stream_->flush();
            } else {
                spdlog::info("[SEND] {}", line);
            }
        };

        // Per-packet header — visual separator + packet-level metadata.
        {
            char hdr[192];
            // First 8 bytes of payload (hex) — correlates with blueprint_bunches.csv.
            char prefix[32] = "";
            size_t show = std::min<size_t>(8, rpkt.raw.size());
            char* wp = prefix;
            for (size_t i = 0; i < show; ++i) {
                wp += std::snprintf(wp, sizeof(prefix) - (wp - prefix),
                                    "%02x ", rpkt.raw[i]);
            }
            std::snprintf(hdr, sizeof(hdr),
                "-- pkt#%u orig_seq=%u bunch_bits=%u start_bit=%u raw=%zuB "
                "first8=%s--",
                sent_seq,
                static_cast<unsigned>(rpkt.original_seq),
                static_cast<unsigned>(rpkt.bunch_bits),
                static_cast<unsigned>(rpkt.bunch_start_bit),
                rpkt.raw.size(),
                prefix);
            emit(hdr);
        }

        size_t b   = rpkt.bunch_start_bit;
        size_t eff = static_cast<size_t>(rpkt.bunch_start_bit)
                   + static_cast<size_t>(rpkt.bunch_bits);
        int idx = 0;
        while (b < eff) {
            aoc::BunchSummary bs;
            bs.pkt_index = sent_seq;
            size_t b_before = b;
            if (!aoc::parse_sc_bunch(rpkt.raw.data(), rpkt.raw.size(),
                                     eff, b, bs)) {
                // Dump the first 16 bits from the failure position as a
                // 4-char hex prefix so we can pattern-spot the bits that
                // choke our parser.
                char hex[20] = "----";
                if (b_before + 16 <= rpkt.raw.size() * 8u) {
                    size_t pos = b_before;
                    uint16_t word = static_cast<uint16_t>(
                        ue5::read_bits(rpkt.raw.data(), rpkt.raw.size(),
                                       pos, 16));
                    std::snprintf(hex, sizeof(hex), "%04x", word);
                }
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                    "pkt#%u bunch#%d PARSE_FAIL at bit %zu (pkt_bits=%u "
                    "next16=0x%s)",
                    sent_seq, idx, b, static_cast<unsigned>(rpkt.bunch_bits),
                    hex);
                emit(msg);
                verbose_parse_fails_++;
                break;
            }

            // DRIFT ALARM — real AoC channels stay under ~1024.  Anything
            // bigger is parser drift from a preceding mis-read.
            if (bs.ch_idx > 1024u) {
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                    "pkt#%u bunch#%d DRIFT: ch_idx=%u is not a real channel "
                    "(bit %zu→%zu) — stopping", sent_seq, idx, bs.ch_idx,
                    b_before, b);
                emit(msg);
                verbose_drift_events_++;
                break;
            }

            // GHOST-BUNCH detection — when the parser drifts into trailing
            // zero padding it happily parses endless "empty" bunches.  Stop
            // on the first zero-data non-control bunch that isn't a real
            // control-close (which can legitimately have bits=0 on ch!=0
            // but ctrl=1).  The combination ch=0 + bits=0 + ctrl=0 is
            // nonsense on the wire.
            if (bs.ch_idx == 0 && bs.bunch_data_bits == 0
                && !bs.b_control && idx > 0) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                    "pkt#%u bunch#%d GHOST: ch=0 bits=0 ctrl=0 — likely "
                    "trailing padding, stopping", sent_seq, idx);
                emit(msg);
                verbose_ghost_events_++;
                break;
            }
// Cyb captures, C.O.A code

            // Collect stats on this successfully-parsed bunch.
            verbose_kind_counts_[static_cast<size_t>(bs.kind)]++;
            verbose_channel_counts_[bs.ch_idx]++;

            // Format one line per bunch.
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "pkt#%u #%d ch=%-4u kind=%-12s bits=%-5u rel=%d part=%d%c%c "
                "exp=%d gui=%d ctrl=%d chSeq=%u name='%s'",
                sent_seq, idx, bs.ch_idx,
                aoc::bunch_kind_name(bs.kind),
                static_cast<unsigned>(bs.bunch_data_bits),
                bs.b_reliable ? 1 : 0,
                bs.b_partial  ? 1 : 0,
                bs.b_partial_initial ? 'I' : '-',
                bs.b_partial_final   ? 'F' : '-',
                bs.b_exports ? 1 : 0,
                bs.b_guids   ? 1 : 0,
                bs.b_control ? 1 : 0,
                static_cast<unsigned>(bs.ch_seq),
                bs.ch_name.c_str());
            emit(buf);
            verbose_logged_bunches_++;
            if (config_.verbose_bunch_limit > 0
                && verbose_logged_bunches_ >= config_.verbose_bunch_limit) {
                emit("-- verbose bunch log limit reached; further bunches silent --");
                break;
            }
            idx++;
        }

        // Periodic mid-session stats flush — every 5000 bunches we emit
        // a snapshot so the user gets summaries even if the server is
        // hard-killed (Ctrl+C / window close) without clean shutdown.
        // Pure append, doesn't close the stream.
        if (verbose_logged_bunches_ > 0
            && verbose_logged_bunches_ % 5000 == 0) {
            flush_verbose_bunch_summary(/*closing=*/false);
        }
    }

    // PHASE B: session statistics dump.  Called periodically (every 5000
    // bunches) AND on clean shutdown (via stop()).  Emits an aggregate
    // snapshot so the user gets usable stats even if the server is
    // hard-killed.  `closing` means this is the final flush — after this
    // returns, we close the dedicated stream.
    void flush_verbose_bunch_summary(bool closing = true) {
        if (!config_.verbose_bunches) return;
        if (verbose_packet_count_ == 0
            && verbose_logged_bunches_ == 0) return;

        auto emit = [&](const char* line) {
            if (verbose_bunch_log_stream_) {
                *verbose_bunch_log_stream_ << line << '\n';
                verbose_bunch_log_stream_->flush();
            } else {
                spdlog::info("[SEND-STATS] {}", line);
            }
        };

        char buf[256];
        emit("");
        emit("================================================================");
        emit("  [SEND] SESSION STATISTICS");
        emit("================================================================");
        std::snprintf(buf, sizeof(buf),
            "  packets logged     : %u", verbose_packet_count_);
        emit(buf);
        std::snprintf(buf, sizeof(buf),
            "  bunches logged     : %u", verbose_logged_bunches_);
        emit(buf);
        std::snprintf(buf, sizeof(buf),
            "  PARSE_FAIL events  : %u", verbose_parse_fails_);
        emit(buf);
        std::snprintf(buf, sizeof(buf),
            "  DRIFT events       : %u", verbose_drift_events_);
        emit(buf);
        std::snprintf(buf, sizeof(buf),
            "  GHOST events       : %u", verbose_ghost_events_);
        emit(buf);

        emit("");
        emit("  KIND distribution:");
        static const char* kind_names[7] = {
            "Control", "ActorOpen", "ActorClose", "GUIDExport",
            "PartialCont", "ActorReliable", "ActorUpdate"
        };
        for (size_t i = 0; i < verbose_kind_counts_.size(); ++i) {
            if (verbose_kind_counts_[i] == 0) continue;
            std::snprintf(buf, sizeof(buf),
                "    %-14s : %u", kind_names[i], verbose_kind_counts_[i]);
            emit(buf);
        }

        emit("");
        emit("  TOP 20 channels:");
        std::vector<std::pair<uint32_t, uint32_t>> top(
            verbose_channel_counts_.begin(), verbose_channel_counts_.end());
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b){ return a.second > b.second; });
        for (size_t i = 0; i < std::min<size_t>(20, top.size()); ++i) {
            std::snprintf(buf, sizeof(buf),
                "    ch=%-5u  %u", top[i].first, top[i].second);
            emit(buf);
        }
        emit("================================================================");

        // Only close the dedicated stream on the FINAL flush.  Periodic
        // snapshots flush in place and keep the stream open.
        if (verbose_bunch_log_stream_) {
            verbose_bunch_log_stream_->flush();
            if (closing) {
                verbose_bunch_log_stream_.reset();
            }
        }
    }

    /// PM36 (2026-04-29) — Path C Phase 1: renumber captured chSeq values
    /// to be consecutive with our session's per-channel tracker.
    ///
    /// Inputs:
    ///   buf, buf_cap       — packet buffer being built
    ///   bunch_start_bit    — absolute bit offset where bunches begin
    ///   bunch_bits         — bunch data length
    ///   cs                 — ClientState (carries last_outgoing_reliable_chseq)
    ///
    /// Walks all reliable bunches via find_chseq_offsets(), patches each
    /// chSeq with cs.last_outgoing_reliable_chseq[ch] + 1.  Updates the
    /// tracker so subsequent bunches on the same ch are contiguous.
    void patch_replay_chseq(uint8_t* buf, size_t buf_cap,
                            size_t bunch_start_bit, size_t bunch_bits,
                            ClientState& cs) {
        if (bunch_bits == 0) return;

        // Extract bunch bits into a byte-aligned temp buffer for find_chseq_offsets.
        const size_t bunch_bytes = (bunch_bits + 7) / 8;
        std::vector<uint8_t> tmp(bunch_bytes, 0);
        for (size_t i = 0; i < bunch_bits; ++i) {
            size_t src = bunch_start_bit + i;
            uint64_t bit = ue5::read_bits(buf, buf_cap, src, 1);
            if (bit) tmp[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }

        // Locate chSeq positions (relative to tmp / bunch start).
        auto patches = find_chseq_offsets(tmp.data(), tmp.size(), bunch_bits);

        // Apply each patch: assign consecutive chseq from our tracker.
        int num_patched = 0;
        for (const auto& p : patches) {
            if (p.skip) continue;
// (Cyb | C.O.A)

            uint32_t ch = p.ch_index;
            auto& tracker = cs.last_outgoing_reliable_chseq[ch];

            uint16_t new_chseq;
            if (tracker == 0) {
                // First reliable bunch we've ever sent on this channel.
                // Start from in_reliable_base + 1 (matches PcEmitter open
                // pattern: ch=3 opens at base+1, then ch=3 increments by 1
                // per subsequent reliable bunch).
                new_chseq = static_cast<uint16_t>((cs.in_reliable_base + 1) & 0x3FF);
                if (new_chseq == 0) new_chseq = 1;  // never assign 0 (uninit sentinel)
            } else {
                new_chseq = static_cast<uint16_t>((tracker + 1) & 0x3FF);
                if (new_chseq == 0) new_chseq = 1;
            }
            tracker = new_chseq;

            // Patch in our buffer at absolute bit offset.  ChSeq is 10 bits
            // (PM20 confirmed via RE of sub_144230D50).
            size_t abs_off = bunch_start_bit + p.bit_offset;
            ue5::patch_bits(buf, buf_cap, abs_off, new_chseq, 10);
            ++num_patched;
        }

        if (num_patched > 0) {
            spdlog::debug("[chSeq-renumber] patched {} of {} chseqs",
                          num_patched, patches.size());
        }
    }

    size_t build_replay_packet(uint8_t* buf, size_t buf_cap,
                               const ReplayPacketInfo& rpkt,
                               ClientState& cs) {
        // Skip empty packets and captured keepalives (no bunch data)
        if (rpkt.raw.empty() || buf_cap < rpkt.raw.size() + 32) return 0;
        if (rpkt.bunch_bits == 0) return 0;  // keepalive — caller sends its own

        // Zero the buffer — write_bits ORs into it
        std::memset(buf, 0, buf_cap);
        size_t off = 0;

        // ── Packet prefix (outer + notify + history + custom field) ───
        write_sc_packet_prefix(buf, buf_cap, off, cs,
                               /*handshake=*/false,
                               rpkt.hist_count);

        // ── PacketInfo (use original packet's values) ───────────────────
        // AoC: hasServerFrameTime is INDEPENDENT of hasPktInfo
        ue5::write_bits(buf, buf_cap, off, rpkt.has_pkt_info, 1);
        if (rpkt.has_pkt_info) {
            ue5::write_bits(buf, buf_cap, off, rpkt.jitter, 10);
        }
        // hasServerFrameTime — always written regardless of hasPktInfo
        ue5::write_bits(buf, buf_cap, off, rpkt.has_srv_frame, 1);
        if (rpkt.has_srv_frame) {
            ue5::write_bits(buf, buf_cap, off, rpkt.frame_time, 8);
        }

        // ── Bunch data (originally verbatim; PM36 Phase 1 — patched) ───
        size_t our_bunch_start = off;  // remember bit offset for patcher
        if (rpkt.bunch_bits > 0 && rpkt.bunch_start_bit > 0) {
            const uint8_t* orig = rpkt.raw.data();
            size_t orig_len = rpkt.raw.size();
            size_t src = rpkt.bunch_start_bit;
            size_t remaining = rpkt.bunch_bits;

            // Copy in 8-bit chunks for speed
            while (remaining >= 8) {
                uint8_t byte = static_cast<uint8_t>(
                    ue5::read_bits(orig, orig_len, src, 8));
                ue5::write_bits(buf, buf_cap, off, byte, 8);
                remaining -= 8;
            }
            // Copy remaining bits
            for (size_t i = 0; i < remaining; ++i) {
                uint64_t bit = ue5::read_bits(orig, orig_len, src, 1);
                ue5::write_bits(buf, buf_cap, off, static_cast<uint32_t>(bit), 1);
            }
        }

        // ── PM36 (2026-04-29) — Path C Phase 1: chSeq RENUMBER ─────────
        //
        // Captured bunches reference chSeq values from THEIR session's
        // per-channel counters.  In NATIVE mode (PcEmitter opens ch=3 at
        // chSeq=954, then captured world data references chSeq=955+
        // assuming captured pkts 22-46 ran), the renumber makes captured
        // bunches contiguous with our native emits → flushes client queue.
        //
        // ── PM41 (2026-04-30) — GATE TO NATIVE MODE ONLY ───────────────
        //
        // The legacy replay path (launch_all.bat, no --native flag) ALIGNS
        // our session's out_seq to the captured initSeq via
        //   "[Replay] Overriding initial_seq=14265 (in_reliable_base=1977)"
        // This makes captured chSeq values flow naturally without renumber.
        //
        // PM36's unconditional renumber BROKE legacy replay because it
        // overwrote those captured-aligned chSeq values with new sequential
        // ones starting from (in_reliable_base+1) & 0x3FF = 954.  Captured
        // expected 1978; client received 954 → 1024-bunch gap → buffered
        // forever or dropped → world never populates.
        //
        // Gate: only renumber when --no-replay-loop is set (native mode).
        // In legacy mode (disable_replay_loop=false), preserve captured
        // chSeq values so the replay's initSeq alignment works as designed.
        //
        // Side effects (native mode only):
        //   - REPLAY-DIAG bit-mismatch is EXPECTED here (we intentionally
        //     differ from captured).  REPLAY-DIAG below will warn but
        //     it's no longer fatal information.
        //   - cs.last_outgoing_reliable_chseq advances for each ch touched.
        if (rpkt.bunch_bits > 0 && config_.disable_replay_loop) {
            patch_replay_chseq(buf, buf_cap, our_bunch_start,
                               rpkt.bunch_bits, cs);
        }
// [Cyb ~ C O A]

        // ── PM18 (2026-04-28) — bit-level diff diagnostic ──
        //
        // To prove or refute the cursor-desync hypothesis, compare our
        // re-emit's bunch bits against the captured packet's bunch bits
        // at the SAME relative offset.  If they differ at bit-i, our
        // prefix has shifted the bunch by some amount → cursor desync
        // on the client side → CNSF.
        //
        // Compares the FIRST min(bunch_bits, 64) bits and logs first diff.
        if (rpkt.bunch_bits > 0 && rpkt.bunch_start_bit > 0) {
            const size_t our_bunch_start = off - rpkt.bunch_bits;
            const size_t cap_bunch_start = rpkt.bunch_start_bit;
            // First sanity: prefix bit count must match captured.
            if (our_bunch_start != cap_bunch_start) {
                spdlog::warn("[REPLAY-DIAG] PREFIX BIT COUNT MISMATCH: "
                             "our_prefix={} cap_prefix={} delta={} "
                             "(hist={} pktInfo={} srvFrame={}) — bunch alignment shifted!",
                             our_bunch_start, cap_bunch_start,
                             static_cast<int64_t>(our_bunch_start) -
                                 static_cast<int64_t>(cap_bunch_start),
                             rpkt.hist_count,
                             static_cast<int>(rpkt.has_pkt_info),
                             static_cast<int>(rpkt.has_srv_frame));
            }
            // PM18b: check ALL bunch bits, not just first 64.
            const size_t cmp_bits = rpkt.bunch_bits;
            bool aligned_match = true;
            size_t first_diff_bit = 0;
            for (size_t i = 0; i < cmp_bits; ++i) {
                size_t pos1 = our_bunch_start + i;
                size_t pos2 = cap_bunch_start + i;
                uint64_t b1 = ue5::read_bits(buf, buf_cap, pos1, 1);
                uint64_t b2 = ue5::read_bits(rpkt.raw.data(), rpkt.raw.size(), pos2, 1);
                if (b1 != b2) {
                    aligned_match = false;
                    first_diff_bit = i;
                    break;
                }
            }
            if (!aligned_match) {
                // PM36 — bit mismatches are now EXPECTED for reliable bunches
                // because patch_replay_chseq overwrote captured chSeq values.
                // We log at debug level (not warn) so the signal isn't lost
                // but the noise doesn't drown the log.
                spdlog::debug("[REPLAY-DIAG] re-emit BUNCH BIT MISMATCH at bit+{} of {}: "
                              "our_bunch_start={} cap_bunch_start={} "
                              "hist={} pktInfo={} srvFrame={} "
                              "(expected: chSeq renumber active)",
                              first_diff_bit, rpkt.bunch_bits, our_bunch_start,
                              cap_bunch_start, rpkt.hist_count,
                              static_cast<int>(rpkt.has_pkt_info),
                              static_cast<int>(rpkt.has_srv_frame));
            }
        }

        // ── Termination ────────────────────────────────────────────────
        // NO sentinel bit — captured bunch data already ends at eff_bits.
        // Adding a 1-bit sentinel here becomes valid data in UE5's eff_bits
        // window: the receiver reads all bunches, then hits the extra 1,
        // interprets it as bControl=1, tries to parse a phantom bunch header,
        // and overflows → BunchHeaderOverflow → FaultDisconnect.
        // (Same fix already applied in build_bootstrap_packet.)
        size_t pkt_len = ue5::add_termination(buf, buf_cap, off);

        // PHASE 2c: verbose bunch logging (no-op unless config enables it).
        log_replay_packet_bunches(rpkt, cs.out_seq);

        // ── PM30 (2026-04-29) — CNSF FORENSIC SCANNER ─────────────────
        // Walk every bunch in the just-built packet using the same parser
        // the AOC client uses (sub_144230D50 + sub_14168B8D0 path).  When
        // we hit a bunch whose ChName field has bIsHardcoded=0 (soft FName)
        // and FString length > 0x1000000 (16MB UE max), we found the bunch
        // that triggers CNSF.  Log replay_idx + bunch number + bit position
        // so we can cross-reference with the client error.
        //
        // PM32: disabled by default — switched to UE5 verbose net logging
        // for ground truth from the client side.  Re-enable by setting
        // kForensicEnabled = true.
        constexpr bool kForensicEnabled = false;
        if (kForensicEnabled)
            cnsf_forensic_scan(buf, pkt_len, rpkt);

        return pkt_len;
    }

    /// PM30 — CNSF forensic scanner.  Parses each bunch in the just-built
    /// packet and flags any with bIsHardcoded=0 + FString length > 16MB.
    /// This identifies the exact bunch the client will choke on.
    void cnsf_forensic_scan(const uint8_t* buf, size_t pkt_len,
                             const ReplayPacketInfo& rpkt) {
        // bunch_data area: starts at bunch_start_bit, length bunch_bits
        const size_t bunch_start = rpkt.bunch_start_bit;
        const size_t bunch_bits  = rpkt.bunch_bits;
        if (bunch_bits == 0 || pkt_len * 8 < bunch_start + bunch_bits)
            return;

        size_t pos = bunch_start;
        const size_t end_bit = bunch_start + bunch_bits;
        int bunch_idx = 0;

        auto serialize_int = [&](uint32_t max_val, size_t& p) -> uint32_t {
            if (max_val <= 1) return 0;
            uint32_t v = 0, mask = 1;
            while (v + mask < max_val && mask != 0) {
                if (p >= end_bit) return v;
                if (ue5::read_bits(buf, pkt_len, p, 1)) v |= mask;
                mask <<= 1;
            }
            return v;
        };

        auto sip = [&](size_t& p) -> uint32_t {
            uint32_t v = 0;
            int sh = 0;
            for (int i = 0; i < 5; ++i) {
                if (p + 8 > end_bit) return v;
                uint32_t b = static_cast<uint32_t>(
                    ue5::read_bits(buf, pkt_len, p, 8));
                v |= ((b >> 1) & 0x7F) << sh;
                if (!(b & 1)) break;
                sh += 7;
            }
            return v;
        };
// Cyb - C | O | A

        while (pos + 20 < end_bit && bunch_idx < 30) {
            size_t bunch_bit_start = pos;
            bool bControl = ue5::read_bits(buf, pkt_len, pos, 1) != 0;
            bool bOpen = false, bClose = false;
            if (bControl) {
                bOpen  = ue5::read_bits(buf, pkt_len, pos, 1) != 0;
                bClose = ue5::read_bits(buf, pkt_len, pos, 1) != 0;
                if (bClose) (void)serialize_int(15, pos); // CloseReason
            }
            (void)ue5::read_bits(buf, pkt_len, pos, 1); // bIsRepPaused
            bool bReliable = ue5::read_bits(buf, pkt_len, pos, 1) != 0;
            // ChIndex via SerializeIntPacked (SIP) when engine_ver >= 3.
            // This matches our find_chseq_offsets and incoming C>S parser
            // (game_server.h line 3878 + 4870-4878).
            // PM14 memory said SerializeInt(MAX=1024) which was a wrong RE.
            uint32_t ch_idx = sip(pos);
            if (ch_idx > 16383) break; // drift
            (void)ue5::read_bits(buf, pkt_len, pos, 1); // bHasPME
            (void)ue5::read_bits(buf, pkt_len, pos, 1); // bHasMBG
            bool bPartial = ue5::read_bits(buf, pkt_len, pos, 1) != 0;
            // PM32 — verbose-logging path: the forensic scanner cannot
            // converge from the wire alone.  We're switching to client-
            // side ground truth via UE5 verbose net logging.  The
            // scanner is left in place but disabled to keep server log
            // clean while we test.  Re-enable kForensicEnabled to debug
            // wire-level issues again.
            uint32_t ch_seq = 0;
            if (bReliable) {
                if (pos + 10 > end_bit) break;
                ch_seq = static_cast<uint32_t>(
                    ue5::read_bits(buf, pkt_len, pos, 10));
            }
            bool bPartialInitial = false;
            if (bPartial) {
                if (pos + 3 > end_bit) break;
                bPartialInitial = ue5::read_bits(buf, pkt_len, pos, 1) != 0;
                (void)ue5::read_bits(buf, pkt_len, pos, 1); // bPartialCEF
                (void)ue5::read_bits(buf, pkt_len, pos, 1); // bPartialFinal
            }
            bool has_chname = (bReliable || bOpen) &&
                              (!bPartial || bPartialInitial);
            if (has_chname) {
                if (pos + 1 > end_bit) break;
                bool bHardcoded = ue5::read_bits(buf, pkt_len, pos, 1) != 0;
                if (!bHardcoded) {
                    if (pos + 32 > end_bit) break;
                    uint32_t len = static_cast<uint32_t>(
                        ue5::read_bits(buf, pkt_len, pos, 32));
                    // The CNSF threshold per UE5 = 16MB (0x1000000)
                    if (len > 0x1000000) {
                        spdlog::error(
                            "[PM30-CNSF] ★★★ FOUND IT! "
                            "bunch_idx={} ch={} ctrl={} open={} close={} "
                            "reliable={} chSeq={} partial={} pInit={} "
                            "bHardcoded=0 FString_len=0x{:08x} ({}) "
                            "@ bit {} (rel {})",
                            bunch_idx, ch_idx, bControl, bOpen, bClose,
                            bReliable, ch_seq, bPartial, bPartialInitial,
                            len, len, bunch_bit_start,
                            bunch_bit_start - bunch_start);
                    }
                    // We can't read the chars / Number here without losing
                    // bunch alignment, so just bail out of this packet
                    // (the rest of bunches are unreachable anyway since
                    // the client would close on this bad name).
                    return;
                } else {
                    (void)sip(pos); // EName index
                }
            }
            // BunchDataBits — fixed 13 bits (matches existing parser at 3933)
            if (pos + 13 > end_bit) break;
            uint32_t bdb = static_cast<uint32_t>(
                ue5::read_bits(buf, pkt_len, pos, 13));
            if (bdb > 8191) break; // drift
            if (pos + bdb > end_bit) {
                // Bunch claims more bits than packet has — this would also
                // confuse the client.  Log it.
                spdlog::warn(
                    "[PM30-OVERFLOW] bunch_idx={} ch={} bdb={} "
                    "remaining={} (packet has insufficient bits)",
                    bunch_idx, ch_idx, bdb, end_bit - pos);
                break;
            }
            pos += bdb;
            ++bunch_idx;
        }
    }

    /// Replay thread: sends captured S>C packets with adjusted headers.
    /// Skips NMT/keepalive packets at the start since NMT was already
    /// negotiated live.  Starts from the first real game data packet.
    /// Phase D1: read generated_channels.json and instantiate one generator
    /// per entry.  Unknown generator names are logged and skipped — an
    /// unrecognised entry is never fatal.  Called once from the constructor.
    void load_generator_config(const std::string& path) {
        std::ifstream f(path);
        if (!f) {
            spdlog::warn("[GEN] Cannot open generator config '{}' — skipping", path);
            return;
        }
        nlohmann::json j;
        try {
            f >> j;
        } catch (const std::exception& e) {
            spdlog::error("[GEN] Failed to parse '{}': {}", path, e.what());
            return;
        }
        if (!j.contains("entries") || !j["entries"].is_array()) {
            spdlog::warn("[GEN] '{}' missing 'entries' array — no generators registered", path);
            return;
        }
        for (const auto& e : j["entries"]) {
            uint32_t ch = e.value("channel", 0u);
            std::string name = e.value("generator", std::string{});
            std::unique_ptr<aoc::BunchGenerator> g;
            if (name == "noop") {
                g = aoc::generators::make_noop_generator(ch);
            } else if (name == "recorded") {
                // D2: single-shot replay of a known-good bunch.
                // Expected JSON fields:
                //   payload_hex   — hex string, whitespace-tolerant
                //                   (e.g. "56 61 08 00" or "56610800")
                //   bit_count     — authoritative bit length of payload
                //   emit_at_ms    — session_ms at which to emit (0 = asap)
                //   ch_name       — optional label for logs
                std::string payload_hex = e.value("payload_hex", std::string{});
                uint32_t bit_count      = e.value("bit_count", 0u);
                uint64_t emit_at_ms     = e.value("emit_at_ms", 0ull);
                std::string label       = e.value("ch_name", std::string{});

                // Parse hex: strip spaces/colons, two chars per byte.
                std::vector<uint8_t> payload;
                payload.reserve(payload_hex.size() / 2);
                uint8_t nyb = 0; bool have = false; bool bad = false;
                for (char c : payload_hex) {
                    if (c == ' ' || c == '\t' || c == ':' || c == '-') continue;
                    uint8_t v;
                    if      (c >= '0' && c <= '9') v = c - '0';
                    else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
                    else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
                    else { bad = true; break; }
                    if (!have) { nyb = v; have = true; }
                    else       { payload.push_back((nyb << 4) | v); have = false; }
                }
                if (bad || have || bit_count == 0 ||
                    payload.size() < (bit_count + 7) / 8) {
                    spdlog::warn("[GEN] ch={} recorded generator: bad config "
                                 "(hex='{}' bytes={} bit_count={}) — skipped",
                                 ch, payload_hex, payload.size(), bit_count);
                    continue;
                }
                g = aoc::generators::make_recorded_generator(
                    ch, std::move(payload), bit_count, emit_at_ms, label);
                spdlog::info("[GEN] Registered recorded ch={} label='{}' "
                             "bits={} emit_at_ms={}",
                             ch, label, bit_count, emit_at_ms);
            } else {
                spdlog::warn("[GEN] Unknown generator '{}' for ch={} — skipped", name, ch);
                continue;
            }
            if (g) {
                if (name != "recorded")
                    spdlog::info("[GEN] Registered generator '{}' on ch={}", name, ch);
                generators_.push_back(std::move(g));
            }
        }
        spdlog::info("[GEN] {} generator(s) active", generators_.size());
    }
// Cyb.C.O.A

    void replay_loop(const std::string& client_key, sockaddr_in client_addr) {
        if (!replay_data_ || replay_data_->packets.empty()) {
            spdlog::error("[Replay] No replay data!");
            return;
        }

        // ── Per-session personalization / synthesis ───────────────────
        // Build a CharacterProfile from whatever live data is available
        // (currently just the character name from XClient).  This profile
        // feeds the RepLayout synthesis path:
        //   BootstrapSequence::apply_synthesis() — replaces entire packet
        //   payloads with builder output (Phase 3.8).
        // When a builder produces byte-identical output to the captured
        // data (Phase 3.7 validated this for PlayerController), synthesis
        // is a no-op in bytes but exercises the pipeline end-to-end.
        // (The old patch_character_name path was removed 2026-04-23 —
        //  see the big "FString patcher REMOVED" comment at ReplayData.)
        aoc::protocol::CharacterProfile profile;
        if (character_name_provider_) {
            profile.name = character_name_provider_();
        }
        if (character_archetype_provider_) {
            profile.archetype_id = character_archetype_provider_();
        }

        // ── Session 3: Multiplayer NetGUID allocation ──
        // Allocate a per-player NetGUID block.  When the PC builder
        // synthesizes bunches, it will use block.player_controller as
        // the actor's unique NetGUID — so multiple connected players
        // don't share the same GUID space.
        //
        // NOTE: The current replay loop still plays back CAPTURED bytes
        // with their ORIGINAL NetGUIDs (ActorNetGuid=1, etc.).  This
        // allocator lays the foundation; the actual use of allocated
        // GUIDs happens when builders fully replace spliced payloads
        // (see BootstrapSequence::apply_synthesis + player_controller::build).
        //
        // For now, we allocate the block but pass only the PC GUID hint
        // through the profile.  When the builder path is exercised via
        // apply_synthesis the allocated GUID takes effect.
        const std::string player_key =
            profile.name.empty() ? "default" : profile.name;
        aoc::protocol::PlayerNetGuidBlock block =
            net_guid_allocator_.get_block(player_key);
        if (!block.is_valid()) {
            block = net_guid_allocator_.allocate_player_block(player_key);
        } else {
            spdlog::info("[GameServer] Reusing NetGUID block for \"{}\" "
                         "(base=0x{:x})", player_key, block.base);
        }
        profile.actor_netguid_hint =
            static_cast<uint32_t>(block.player_controller);
        // Future: also set profile.pawn_netguid_hint,
        // profile.player_state_netguid_hint, etc. when CharacterProfile
        // grows those fields and builders consume them.
        //
        // The character-name and pawn-nametag FString patchers were removed
        // (2026-04-23) — they tried to rewrite captured bunch bytes in
        // place, which was fundamentally fragile (cross-packet dependencies,
        // mask/export drift).  Names now flow through the RepLayout
        // synthesizer at emit time; see src/protocol/emit/.  Until that
        // synthesizer is wired into the live path, the replay stream
        // renders with its captured name ("RandomChar").
        if (!profile.name.empty()) {
            spdlog::info("[Replay] live character name=\"{}\" "
                         "(synthesizer will apply at emit time)",
                         profile.name);
        }

        // (M2.1 RandomChar byte patcher removed 2026-04-26 — was a dead-end
        //  hack tied to captured bytes; doesn't survive a real authoritative
        //  pipeline.  custom_name is still consumed by PcEmitter / native
        //  sequencer for the actual property emit path.)

        // Tier-1 property patcher moved to GameServer constructor (runs ONCE
        // at startup on replay_data_ regardless of source — embedded or file).
        // See ctor around line ~960 for the single-instance patcher block.

        // Phase M5.7 archetype-bit patching was disabled after it broke
        // HUD/hotbars/map; investigation log in docs/phase-m5-property-
        // decoding.md.  The live path builds the PlayerController bunch
        // from the ActorBuilder (Session C) instead, which gets the class
        // right by construction.  profile.archetype_id is consumed there,
        // not here.
        (void)profile.archetype_id;
// (C:Y:B : CoA)

        // Phase 3.8: invoke PlayerController builder and splice its
        // synthesized bunch into the packet stream.  With an empty or
        // default profile this produces byte-identical output to the
        // capture (Phase 3.7 guarantee), so there's no wire change.
        // Failure here is logged but non-fatal — replay continues with
        // the captured bunch bytes (i.e. "RandomChar") unchanged.
        //
        // Session H.4: when --live-pc-spawn is set, we ALSO run the modern
        // emitter over pkt[22] — it rewrites the full 4905-bit bunch
        // (not just the legacy 3302-bit payload) using ActorBuilder +
        // PackageMapExporter + spliced RepLayout tail.  Output is
        // bit-identical with captured, so wire behavior is preserved.
        // This is the validation step that proves our emitter integrates
        // correctly into the live replay pipeline — once confirmed via
        // real-client test, H.4e+ can diverge from captured safely.
        aoc::protocol::BootstrapSequence::apply_synthesis(
            *replay_data_, profile);

        // (Removed: apply_live_pc_spawn / apply_live_pawn_spawn calls.
        //  Phase II replaces this with spawn_player_controller_for_client.
        //  The Stage 2.0 round-trip test moved to startup.)

        // ── Replay from the very first data packet ────────────────────
        // We start the replay AFTER NMT_GameSpecific is received (live NMT
        // phase is fully complete).  The captured session's early NMT bunches
        // (channel 0, small packets) will arrive with reliable-seqs the client
        // already processed — UE5 silently drops them as old retransmits.
        // Starting from index 0 ensures ALL channel-open bunches for actor
        // channels arrive before any data bunches for those channels, preventing
        // BunchBadChannelIndex errors.
        // NOTE: packets with bb==0 (keepalives) are skipped by build_replay_packet.
        size_t start_idx = 0;

        spdlog::warn("[Replay] ========================================");
        spdlog::warn("[Replay]  REPLAY MODE STARTING (after NMT_GameSpecific)");
        spdlog::warn("[Replay]  Total packets: {}", replay_data_->packets.size());
        spdlog::warn("[Replay]  Starting from index 0 (full replay)");
        spdlog::warn("[Replay]  First pkt: {}B bsb={} bb={}",
                     replay_data_->packets[0].raw.size(),
                     replay_data_->packets[0].bunch_start_bit,
                     replay_data_->packets[0].bunch_bits);
        // ── Fix #32: Adaptive pacing (no hard FC pause) ──────────────
        // Previous approach (Fix #30/31): send MAX_AHEAD packets fast,
        // then completely stop until client acks.  This caused actor
        // channel timeouts (~5s) because the server sent NOTHING during
        // the stall — actor channels opened in the burst never got
        // their continuation data.
        //
        // Fix #32: NEVER stop sending.  Instead, slow down gradually:
        //   ahead < 10:  5ms delay (~200 pkt/s max)
        //   ahead 10-19: 20ms delay (~50 pkt/s)
        //   ahead 20-29: 50ms delay (~20 pkt/s)
        //   ahead 30-39: 200ms delay (~5 pkt/s)
        //   ahead >= 40: 500ms delay (~2 pkt/s)
        //
        // This ensures actor channels always get continuation data
        // even during slow client ticks (loading screen).  The OS UDP
        // buffer (~65KB ≈ 67 packets) won't overflow because at 500ms
        // between packets, only ~10 more accumulate during a 5s tick.
        //
        // SO_SNDBUF=1MB (set at socket creation) prevents send failures.
        // Fix #36: Wait for the client to finish loading the map.
        //
        // Background: the AoC client sends NMT_GameSpecific(18) BEFORE its game
        // thread finishes LoadMap().  If we start sending replay packets
        // immediately, the OS UDP receive buffer fills up while the client's
        // game thread is blocked → packets are dropped → PlayerController spawn
        // and the first N actor channel opens are never processed → player spawns
        // underwater at world-origin with an empty world.
        //
        // Solution: hold here, keep the connection alive with keepalives, and only
        // start the actual replay once the client's game thread is running.
        // The signal is the first C>S packet with bunch data on a non-control
        // channel (set by the DATA handler when it sees a non-NMT bunch after
        // NMT_GameSpecific).  A 120-second safety fallback fires if for some
        // reason the signal never arrives.
        spdlog::warn("[Replay]  Waiting for client to finish map load (Fix #36)...");
        {
            auto wait_start = std::chrono::steady_clock::now();
            constexpr int LOAD_WAIT_TIMEOUT_S = 120;
            while (running_ && replay_active_.load() && !replay_map_loaded_.load()) {
                // Keep the connection alive
                {
                    std::lock_guard<std::mutex> lk(client_mu_);
                    auto it = clients_.find(client_key);
                    if (it == clients_.end() || it->second.phase < ClientState::CONNECTED) {
                        spdlog::warn("[Replay]  Client gone during map-load wait — aborting");
                        return;
                    }
                    send_keepalive(it->second, client_addr, client_key);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - wait_start).count();
                if (secs >= LOAD_WAIT_TIMEOUT_S) {
                    spdlog::warn("[Replay]  Map-load wait timeout ({}s) — starting anyway",
                                 LOAD_WAIT_TIMEOUT_S);
                    break;
                }
            }
        }
        if (!running_ || !replay_active_.load()) return;
        spdlog::warn("[Replay]  Map load complete — starting replay now");
// CoA / C-Y-B

        // Phase D1: reset generator clock to coincide with first outbound send
        gen_session_start_ = std::chrono::steady_clock::now();
        gen_tick_counter_ = 0;

        spdlog::warn("[Replay]  Pacing: hard-block until ahead<200 (tiers: 1ms/5ms/15ms/50ms)");
        spdlog::warn("[Replay] ========================================");

        // Allocate a dynamic buffer sized to the largest replay packet.
        // A fixed 4096-byte buffer was previously too small and silently
        // dropped ~99% of packets > 4KB.
        size_t max_raw = 0;
        for (const auto& p : replay_data_->packets)
            max_raw = std::max(max_raw, p.raw.size());
        size_t buf_cap = max_raw + 512;  // headers + padding
        std::vector<uint8_t> buf_vec(buf_cap, 0);
        uint8_t* buf = buf_vec.data();
        spdlog::warn("[Replay]  Max packet raw: {}B, buffer: {}B", max_raw, buf_cap);

        size_t sent_count = 0;
        size_t error_count = 0;
        size_t skip_count = 0;
        auto last_keepalive_time = std::chrono::steady_clock::now();

        // ── Fix #37: Window-aware pacing ─────────────────────────────────────
        // Root cause of "client stops ACKing" (logs 2026-04-18):
        //   UE5 FNetPacketNotify drops any S>C packet with
        //   seq > last_acked_seq + MAX_SEQUENCE_HISTORY_LENGTH (256).
        //   Old code let ahead reach 4000; packets 257+ were silently dropped.
        //   Result: client processes the first ~256 replay packets, then
        //   stops because everything else falls outside its receive window.
        //
        // Fix: never let ahead exceed MAX_WINDOW (200, 56 below UE5's 256
        // hard limit).  Stop condition: if ahead >= STALL_AHEAD and cliAck
        // has not advanced for STALL_TIMEOUT_S seconds, the client is dead
        // (e.g. the client process was killed).
        // While stalled we still send keepalives every 2s so the UE5
        // connection survives short pauses (map transitions, a dialog, etc.)
        constexpr int MAX_WINDOW      = 200;  // send ceiling (below UE5 256)
        constexpr int STALL_AHEAD     = 150;  // start stall timer here
        constexpr int STALL_TIMEOUT_S = 120;  // give up after 120 s of no ack

        uint16_t last_client_ack = 0;
        bool     last_ack_set    = false;
        auto     last_ack_time   = std::chrono::steady_clock::now();

        // Honor --replay-max-packets: truncate the replay after N packets
        // so we can test whether the bootstrap subset alone is enough for
        // the client to enter the world.  0 = no cap (send everything).
        size_t max_packets = replay_data_->packets.size();
        if (config_.replay_max_packets > 0
            && config_.replay_max_packets < max_packets - start_idx) {
            max_packets = start_idx + config_.replay_max_packets;
            spdlog::warn("[Replay]  --replay-max-packets: truncating to {} packets "
                         "(first {} after start_idx={})",
                         max_packets, config_.replay_max_packets, start_idx);
        }

        // NOTE (2026-04-24): Removed the custom-name injection-into-replay
        // experiment.  After 3 live-test iterations the client kept silently
        // rejecting our injected bunches (the wire format for delta bunches
        // has more structural requirements than "mid-bunch region verbatim"
        // or "bHasRepLayoutExport + NumGUIDs + cmd_index").
        //
        // Decision: stop patching the replay.  The real path forward is the
        // authoritative-server flow (see NativeConnectSequencer).  Replay
        // remains here as a reference / debug tool but `custom_name` is now
        // honored only by the native flow.

        for (size_t i = start_idx; i < max_packets && running_ && replay_active_.load(); ++i) {
            const auto& rpkt = replay_data_->packets[i];

            // NOTE: In-place mutation of pkt#104 (HUD name) / pkt#79 (Pawn
            // nametag) via ReplayMutator was REMOVED 2026-04-23.  Reason:
            // both packets carry their first bunch with bPartialInitial=1
            // — they are the opening fragment of a multi-packet logical
            // bunch.  Changing a single fragment's size shifts the
            // reassembled total, and AoC's client validates something
            // about that total that we can't reach via bit-level
            // mutation.  10-char same-length mutations worked; any
            // length change failed with "loading screen + broken world
            // flash" — the same failure mode the old patcher hit.
            //
            // The library (src/protocol/emit/replay_mutator.*) + its
            // tests (src/tools/test_replay_mutator.cpp) stay in tree —
            // they're clean, validated, and useful as a reference for
            // Phase III (full live-bunch synthesis).
            //
            // Replay now plays back captured bytes VERBATIM.  The
            // character name shown in-game is whatever the replay
            // captured ("RandomChar").  Custom names require Phase III.
// C_Y_B x CoA

            // ── Adaptive pre-send delay + hard window block ────────────
            // IMPORTANT: this block must reduce ahead to < MAX_WINDOW
            // BEFORE the data packet below is sent.  The old approach
            // (just sleeping 100ms then sending anyway) let ahead grow
            // to 1300+ during a stall, wasting bandwidth and
            // filling OS UDP buffers with packets UE5 will drop (>256 ahead).
            // Now we SPIN here (keepalives only) until ahead < MAX_WINDOW
            // or the stall timeout fires.
            {
                // Helper lambda: read ahead + update stall tracker under lock.
                // Returns {ahead, stop_requested}.
                auto read_state = [&]() -> std::pair<int,bool> {
                    std::lock_guard<std::mutex> lk(client_mu_);
                    auto it = clients_.find(client_key);
                    if (it == clients_.end()) return {0, true};
                    auto& cs = it->second;
                    if (cs.phase < ClientState::CONNECTED) {
                        spdlog::warn("[Replay] Client disconnected (phase={}), stopping",
                                     (int)cs.phase);
                        return {0, true};
                    }
                    int a = static_cast<int>(
                        (cs.out_seq - cs.out_ack_seq) & ClientState::SEQ_MASK);
                    if (a > 8192) a -= 16384;
                    if (!last_ack_set || cs.out_ack_seq != last_client_ack) {
                        last_client_ack = cs.out_ack_seq;
                        last_ack_set    = true;
                        last_ack_time   = std::chrono::steady_clock::now();
                    }
                    return {a, false};
                };

                // ── Hard window block: wait until ahead < MAX_WINDOW ──
                // While blocked, send keepalives every 2s so UE5 doesn't
                // idle-timeout (~30s without any UDP from server).
                bool stop = false;
                while (running_ && replay_active_.load()) {
                    auto [ahead_now, stop_req] = read_state();
                    if (stop_req) { stop = true; break; }

                    // Stall detection: game thread frozen
                    if (ahead_now >= STALL_AHEAD) {
                        auto stall_s = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - last_ack_time).count();
                        if (stall_s >= STALL_TIMEOUT_S) {
                            spdlog::warn("[Replay] Client stalled (ahead={}, {}s no ack), stopping",
                                         ahead_now, stall_s);
                            stop = true;
                            break;
                        }
                    }

                    if (ahead_now < MAX_WINDOW) break;  // safe to send next packet

                    // Window full — send keepalive and wait
                    {
                        auto now = std::chrono::steady_clock::now();
                        auto since_ka = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - last_keepalive_time).count();
                        if (since_ka >= 2000) {
                            std::lock_guard<std::mutex> lk(client_mu_);
                            auto it = clients_.find(client_key);
                            if (it != clients_.end()) {
                                send_keepalive(it->second, client_addr, client_key);
                                last_keepalive_time = now;
                            }
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (stop) break;

                // ── Fine-grained delay within the window ─────────────
                // Now ahead < MAX_WINDOW.  Apply tiered delay so we don't
                // immediately fill the window on the next iteration.
                int ahead = 0;
                {
                    auto [a, s] = read_state();
                    if (s) break;
                    ahead = a;
                }

                int delay_ms = 1;
                if      (ahead >= 150) delay_ms = 50;   // ~20 pkt/s
                else if (ahead >= 100) delay_ms = 15;   // ~67 pkt/s
                else if (ahead >= 50)  delay_ms =  5;   // ~200 pkt/s
                // < 50: 1ms burst
// -- C.Y.B . C.O.A --

                // Log pacing transitions
                {
                    static int last_logged_delay = -1;
                    if (delay_ms != last_logged_delay) {
                        spdlog::info("[Replay] Pacing: {}ms (ahead={}, sent={})",
                                     delay_ms, ahead, sent_count);
                        last_logged_delay = delay_ms;
                    }
                }

                // Keepalive at slow tiers (extra safety)
                if (delay_ms >= 50) {
                    auto now = std::chrono::steady_clock::now();
                    auto since_ka = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_keepalive_time).count();
                    if (since_ka >= 2000) {
                        std::lock_guard<std::mutex> lk(client_mu_);
                        auto it = clients_.find(client_key);
                        if (it != clients_.end()) {
                            send_keepalive(it->second, client_addr, client_key);
                            last_keepalive_time = now;
                        }
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }

            // ── Phase D2: pull-tick every registered generator ────────────
            // Invoked once per replay-packet iteration (same cadence as the
            // outbound builder).  If a generator emits, we frame+send it as
            // its own S>C data packet (advancing out_seq).  Replay for this
            // tick continues normally below — generator-emitted packets and
            // replay packets share the same outbound seq counter, so the
            // client sees them in order with no gaps.
            if (!generators_.empty()) {
                aoc::TickContext tctx;
                tctx.tick_index = gen_tick_counter_++;
                auto elapsed = std::chrono::steady_clock::now() - gen_session_start_;
                tctx.session_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
                {
                    std::lock_guard<std::mutex> lk(client_mu_);
                    auto it = clients_.find(client_key);
                    if (it != clients_.end()) {
                        tctx.out_seq_preview = it->second.out_seq;
                        tctx.last_client_ack = it->second.out_ack_seq;
                    }
                }
                for (auto& g : generators_) {
                    auto maybe = g->tick(tctx);
                    if (!maybe.has_value()) continue;
                    if (maybe->channel != g->channel()) {
                        spdlog::warn("[GEN] channel mismatch: generator '{}' owns ch={} "
                                     "but emitted ch={} — dropping",
                                     g->name(), g->channel(), maybe->channel);
                        continue;
                    }
                    std::lock_guard<std::mutex> lk(client_mu_);
                    auto it = clients_.find(client_key);
                    if (it == clients_.end() ||
                        it->second.phase < ClientState::CONNECTED) continue;
                    send_generated_bunch(it->second, client_addr, client_key,
                                         *maybe, g->name());
                }
            }

            // Build the replay packet with rewritten headers
            std::memset(buf, 0, buf_cap);  // clear for write_bits ORing
            size_t pkt_len = 0;
            uint16_t pkt_seq = 0, pkt_ack = 0, client_ack = 0;

            {
                std::lock_guard<std::mutex> lk(client_mu_);
                auto it = clients_.find(client_key);
                if (it == clients_.end() || it->second.phase < ClientState::CONNECTED) {
                    spdlog::info("[Replay] Client {} gone or disconnected, stopping", client_key);
                    break;
                }
                auto& cs = it->second;
                pkt_seq = cs.out_seq;
                pkt_ack = cs.in_ack_seq;
                client_ack = cs.out_ack_seq;
                pkt_len = build_replay_packet(buf, buf_cap, rpkt, cs);
                // Only advance seq on successful builds — advancing on
                // failure causes phantom `ahead` growth and premature
                // slow-pacing throttles.
                if (pkt_len > 0) {
                    cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;
                } else {
                    ++skip_count;
                }
            }

            if (pkt_len > 0) {
                int sent = send_to_client(buf, pkt_len, client_addr);
                if (sent > 0) {
                    ++sent_count;
                    last_keepalive_time = std::chrono::steady_clock::now();
                    int ahead = static_cast<int>((pkt_seq - client_ack) & 0x3FFF);
                    if (ahead > 8192) ahead -= 16384;
                    // Log first 300, then every 1000
                    if (sent_count <= 100 || sent_count % 500 == 0)
                        spdlog::info("[REPLAY] >> #{} {}B seq={} ack={} cliAck={} ahead={} total={}",
                                     i + 1, pkt_len, pkt_seq, pkt_ack, client_ack, ahead, sent_count);

                } else {
                    ++error_count;
                    if (error_count <= 5)
                        spdlog::warn("[Replay] Send failed at #{}", i + 1);
                }
            }
        }

        // Fix #35: clear replay flag so periodic keepalives can resume
        replay_active_.store(false);

        // Level-visibility acks and a ClientRestart held back while the replay was running.
        {
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(client_key);
            if (it != clients_.end() && it->second.phase >= ClientState::CONNECTED) {
                ClientState& cs = it->second;
                if (!cs.pending_sulv_acks.empty()) {
                    auto queue = std::move(cs.pending_sulv_acks);
                    cs.pending_sulv_acks.clear();
                    spdlog::info("[Replay] Sending {} queued ClientAckUpdateLevelVisibility",
                                 queue.size());
                    for (const auto& ack : queue)
                        (void)send_client_ack_update_level_visibility(
                            cs, client_addr, client_key, ack.request, ack.triggering_ch_idx);
                }
                if (cs.client_restart_pending) {
                    cs.client_restart_pending = false;
                    spdlog::info("[Replay] Sending queued ClientRestart");
                    (void)send_client_restart(cs, client_addr, client_key);
                }
            }
        }

        spdlog::warn("[Replay] ========================================");
        spdlog::warn("[Replay]  REPLAY COMPLETE");
        spdlog::warn("[Replay]  Sent: {} / {} (skipped {} NMT, {} build-fail)", sent_count,
                     replay_data_->packets.size() - start_idx, start_idx, skip_count);
        spdlog::warn("[Replay]  Errors: {}", error_count);

        // ── Path X / V3 — synthetic property update bunch (2026-04-26) ──
        // After replay finishes, emit a single bunch with property updates
        // using the CORRECT content-block-framed wire format (per RE).
        //
        // Knobs in config_.v3_*:
        //   v3_emit_enabled  — must be true to fire
        //   v3_target_channel — which channel hosts our PC (try 3 first)
        //   v3_num_properties — class NumReplicated (try 256 first)
        //   v3_reliable       — false safer for first iteration
        //   v3_cmd_handle_*   — per-property handle (iterate to discover)
        //
        // V1/V2 (inject_custom_name_bunch) used wrong wire format.  V3
        // uses bOutermostEnd + bIsChannelActor + NumPayloadBits framing
        // per ProcessBunch RE.
        if (config_.v3_emit_enabled) {
            spdlog::warn("[Replay][V3] Firing synthetic property-update "
                         "bunch (ch={} num_props={} reliable={})",
                         config_.v3_target_channel,
                         config_.v3_num_properties, config_.v3_reliable);
            inject_v3_property_update(client_key, client_addr);
        }
// [C_Y_B x C.O.A]

        // ── Native Name update emission (V1/V2 — DISABLED, kept for reference) ──
        // The inject_custom_name_bunch() V1/V2 used a wrong format that
        // bypassed the content-block routing.  Superseded by V3 above.
        // Code remains for reference; not called.
        // if (!config_.custom_name.empty()) {
        //     inject_custom_name_bunch(client_key, client_addr);  // disabled
        // }

        spdlog::warn("[Replay]  Entering post-replay keepalive loop — session will stay alive.");
        spdlog::warn("[Replay] ========================================");

        // ── Post-replay keepalive loop ────────────────────────────────────────
        // After the replay finishes the world state is frozen but the client is
        // still connected and running client-side movement prediction.  We keep
        // the UE5 connection alive by sending an empty data packet every 200ms.
        // Loop is gated on running_ (server lifetime), NOT replay_active_
        // which is false here — otherwise the connection dies in 20s.
        uint32_t ka_count = 0;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(client_key);
            if (it == clients_.end() || it->second.phase < ClientState::CONNECTED) {
                spdlog::info("[Replay] Post-replay: client disconnected after {} keepalives", ka_count);
                break;
            }
            send_keepalive(it->second, client_addr, client_key);
            ++ka_count;
            if (ka_count % 150 == 0)  // log every 30s
                spdlog::info("[Replay] Post-replay: alive, {} keepalives sent (~{}s)",
                             ka_count, ka_count / 5);
        }
    }

    /// Send a keep-alive (empty data packet) acknowledging the client's sequence.
    /// Phase III M1: build and send a native non-partial property-update
    /// bunch carrying the custom Name.  Called from replay_loop right after
    /// the captured PC ActorOpen (pkt#22) has been delivered — by this
    /// point the client's actor channel is open and ready for deltas.
    ///
    /// Returns true on successful send.
    bool inject_custom_name_bunch(const std::string& client_key,
                                   const sockaddr_in& client_addr) {
        // 1. Build the native bunch via PropertyUpdateBunchBuilder.
        //    Bunch targets channel 3 (PlayerController's actor channel,
        //    matching the captured pkt#22 structure).
        aoc::protocol::emit::PropertyUpdateBunchBuilder b;
        b.set_channel(3);

        // ── Match CAPTURED pkt#104 bunch format (2026-04-24 RE) ───────
        // decode_pkt104_bunch_header.py revealed the captured Name update
        // bunch structure on ch=3:
        //
        //   ch=3  reliable=0  ctrl=0  partial=0
        //   has_pme=0  has_mbg=1  chSeq=0 (no chSeq tracking when !reliable)
        //   bdb=4593 bits  (contains Name + many other prop updates)
        //
        // Crucially:  reliable=0  (NOT reliable!).  UE5 reliable-channel
        // protocol requires exact chSeq+1 ordering; captured AoC skips
        // that for Name updates by sending them non-reliable.  Our
        // earlier reliable=1 sends were being buffered/discarded because
        // chSeq didn't match the client's expected next-value.
        //
        // Non-reliable has no chSeq tracking; we set it to 0.
        b.set_ch_sequence(0);
        b.set_reliable(false);

        // V2 emitter: proper property-delta bunch payload.  Try cmd_index
        // 0x6A first (observed in captured pkt#104 at the Name slot —
        // empirically what the wire uses, even though we don't know its
        // exact semantic meaning).  If that fails, we'll retry with 28.
        constexpr uint32_t kNameCmdIndex = 0x6A;
        b.add_name_update_v2(config_.custom_name, kNameCmdIndex);

        aoc::protocol::emit::BunchWriter bunch;
        size_t bunch_bits = b.build(bunch);
        if (bunch_bits == 0) {
            spdlog::warn("[Replay] inject_custom_name_bunch: empty bunch");
            return false;
        }

        // 2. Wrap in a full UDP packet (same outer framing as other S>C pkts).
        uint8_t buf[1024] = {};
        size_t  off  = 0;

        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(client_key);
        if (it == clients_.end()) return false;
        ClientState& cs = it->second;

        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);

        // PacketInfo: hasPktInfo=1 (required), hasSrvFrame=0
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);   // bHasPacketInfo
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10);  // jitter=max
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);   // bHasServerFrameTime

        // Append the bunch bits.
        for (size_t i = 0; i < bunch_bits; ++i) {
            int bit = (bunch.data()[i >> 3] >> (i & 7)) & 1;
            ue5::write_bits(buf, sizeof(buf), off, bit, 1);
        }

        // ── EXTRA SENTINEL BIT (AoC PacketHandler requirement, Phase B.0p5) ──
        // Same fix as send_bunch_packet, send_ch0_reliable_payload, etc.
        // Without it the client logs "0's in last byte" and silently drops
        // the packet → Pawn name update never reaches the actor channel.
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);

        uint16_t pkt_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        int sent = send_to_client(buf, pkt_len, client_addr);
        if (sent > 0) {
            spdlog::info("[Replay] Injected Name-update bunch: seq={} "
                          "{}B bunch={}bits name=\"{}\"",
                          pkt_seq, pkt_len, bunch_bits, config_.custom_name);
            return true;
        }
        spdlog::warn("[Replay] inject_custom_name_bunch: send_to_client failed");
        return false;
    }
// C | Y | B / C O A

    /// Path X / V3 — inject a synthetic property-update bunch using the
    /// CORRECT content-block-framed wire format (per RE 2026-04-26).
    ///
    /// Wire format (vs. earlier V1/V2 which were wrong):
    ///   Bunch header (PropertyUpdateBunchBuilder writes)
    ///   [1 bit bOutermostEnd=0][1 bit bIsChannelActor=1]
    ///   [SIP NumPayloadBits][Inner bunch:]
    ///     for each prop: [SerializeInt(NumProps) cmd_handle][SIP NumBits][value bits]
    ///   [1 bit bOutermostEnd=1]   ← end marker
    ///
    /// Knobs from config_.v3_*:
    ///   v3_target_channel       — which channel hosts our PC
    ///   v3_num_properties       — class NumReplicated (for cmd_handle bit width)
    ///   v3_reliable             — bunch reliability flag
    ///   v3_cmd_handle_*         — cmd_handle for each property
    ///
    /// Returns true on successful send.
    bool inject_v3_property_update(const std::string& client_key,
                                    const sockaddr_in& client_addr) {
        aoc::protocol::emit::PropertyUpdateBunchBuilder b;
        b.set_channel(config_.v3_target_channel);
        b.set_reliable(config_.v3_reliable);
        b.set_has_mbg(config_.v3_has_mbg);
        // EMPIRICAL: every captured ch=3 PlayerState (GUID 7193) update sets
        // bIsRepPaused=1.  Auto-set when targeting a subobject — overridable
        // via --v3-rep-paused for testing other configurations.
        const bool auto_rp = (config_.v3_subobject_guid != 0);
        b.set_is_rep_paused(config_.v3_rep_paused_override >= 0
                            ? (config_.v3_rep_paused_override != 0)
                            : auto_rp);
        b.set_use_modern_inner_format(config_.v3_use_modern_format);

        // Determine ChSeq if reliable
        uint32_t chseq = 0;
        {
            std::lock_guard<std::mutex> lk(client_mu_);
            auto it = clients_.find(client_key);
            if (it == clients_.end()) return false;
            ClientState& cs = it->second;
            if (config_.v3_reliable) {
                chseq = cs.reliable_seq;
                cs.reliable_seq = (cs.reliable_seq + 1) & 0xFFF;
            }
        }
        b.set_ch_sequence(chseq);

        // Open V3 content block: either targeting the channel's root actor,
        // or a subobject (PlayerState etc.) addressed by SIP NetGUID.
        if (config_.v3_subobject_guid != 0) {
            b.v3_begin_content_block_subobject(config_.v3_subobject_guid,
                                               config_.v3_num_properties);
        } else {
            b.v3_begin_content_block_channel_actor(config_.v3_num_properties);
        }

        int props_added = 0;

        // ── TEST MODE OVERRIDE ───────────────────────────────────────────
        // When v3_test_mode is set, IGNORE all other property config and
        // emit a single known property whose client-side effect is highly
        // visible.  Used to isolate "does V3 wire format work at all?"
        // from "does Name specifically trigger HUD update?"
        const std::string& effective_name = config_.v3_custom_name.empty()
            ? config_.custom_name
            : config_.v3_custom_name;
        if (config_.v3_test_mode == "spectator") {
            // bIsSpectator at cmd_handle=3 (verified-from-code: PlayerState
            // PropertyLinks pos 3 in CPF_Net subset).  1-bit bool.
            b.v3_add_property_bool(3, true);
            ++props_added;
            spdlog::warn("[Replay][V3] TEST MODE: spectator (handle=3 bool=true)");
        } else if (config_.v3_test_mode == "score") {
            // Score at cmd_handle=1 (verified-from-code).  32-bit int32.
            b.v3_add_property_int32(1, 99999);
            ++props_added;
            spdlog::warn("[Replay][V3] TEST MODE: score (handle=1 int32=99999)");
        } else if (config_.v3_test_mode == "pvpoptin") {
            // bRemovePVPOptInProtection at cmd_handle=1 on UAoCStatsComponent.
            // VERIFIED-FROM-CODE: GUID 7193 = UAoCStatsComponent (binary RE,
            // see RE-AOC-CLASSES.md).  Handle 1 = bRemovePVPOptInProtection
            // (RepNotify bool).  If V3 modern format works end-to-end, this
            // bunch reaches the property handler and OnRep_RemovePVPOptInProtection
            // fires.  Effect may or may not be visible depending on PVP state.
            b.v3_add_property_bool(1, true);
            ++props_added;
            spdlog::warn("[Replay][V3] TEST MODE: pvpoptin (handle=1 bool=true on UAoCStatsComponent)");
        } else {
            // Default path — Name update via existing v3_cmd_handle_name + effective_name.
            if (!effective_name.empty() && config_.v3_cmd_handle_name != 0) {
                b.v3_add_property_fstring(config_.v3_cmd_handle_name,
                                          effective_name);
                ++props_added;
            }
        }

        // Level/HP/Gold updates — ONLY in default mode (when test_mode is
        // empty).  In test_mode we want JUST the one test property, isolated.
        if (config_.v3_test_mode.empty()) {
            // Level update (only if custom_level >= 0)
            if (config_.custom_level >= 0 && config_.v3_cmd_handle_level != 0) {
                b.v3_add_property_int32(config_.v3_cmd_handle_level,
                                        config_.custom_level);
                ++props_added;
            }

            // Health updates (cast to float — captured stats are float per GAS)
            if (config_.custom_hp_current >= 0 && config_.v3_cmd_handle_health != 0) {
                b.v3_add_property_float(config_.v3_cmd_handle_health,
                                        static_cast<float>(config_.custom_hp_current));
                ++props_added;
            }
            if (config_.custom_hp_max >= 0 && config_.v3_cmd_handle_max_health != 0) {
                b.v3_add_property_float(config_.v3_cmd_handle_max_health,
                                        static_cast<float>(config_.custom_hp_max));
                ++props_added;
            }
// -- C_Y_B & C-O-A --

            // Gold (int32)
            if (config_.custom_gold >= 0 && config_.v3_cmd_handle_gold != 0) {
                b.v3_add_property_int32(config_.v3_cmd_handle_gold,
                                        config_.custom_gold);
                ++props_added;
            }
        }

        if (props_added == 0) {
            spdlog::info("[Replay][V3] No properties to emit (all custom_*=-1 or cmd_handle=0)");
            return false;
        }

        // Close content block + finalize bunch
        b.v3_end_content_block();
        b.v3_finish_bunch();

        // Build the bunch
        aoc::protocol::emit::BunchWriter bunch;
        size_t bunch_bits = b.build(bunch);
        if (bunch_bits == 0) {
            spdlog::warn("[Replay][V3] Empty bunch produced");
            return false;
        }

        // Wrap in UDP packet (same as inject_custom_name_bunch)
        uint8_t buf[1024] = {};
        size_t  off = 0;

        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(client_key);
        if (it == clients_.end()) return false;
        ClientState& cs = it->second;

        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);   // bHasPacketInfo
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10);  // jitter=max
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);   // bHasServerFrameTime

        for (size_t i = 0; i < bunch_bits; ++i) {
            int bit = (bunch.data()[i >> 3] >> (i & 7)) & 1;
            ue5::write_bits(buf, sizeof(buf), off, bit, 1);
        }

        // ── EXTRA SENTINEL BIT (AoC PacketHandler requirement, Phase B.0p5) ──
        // Same fix as inject_custom_name_bunch / send_bunch_packet / etc.
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);
        uint16_t pkt_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        int sent = send_to_client(buf, pkt_len, client_addr);
        if (sent > 0) {
            const std::string target_desc = (config_.v3_subobject_guid != 0)
                ? fmt::format("ch={} subobj_guid={}", config_.v3_target_channel,
                              config_.v3_subobject_guid)
                : fmt::format("ch={} root", config_.v3_target_channel);
            const bool effective_rp = (config_.v3_rep_paused_override >= 0)
                ? (config_.v3_rep_paused_override != 0) : auto_rp;
            spdlog::warn("[Replay][V3] ★ Injected property-update bunch: "
                         "seq={} {}B bunch={}bits target=[{}] reliable={} "
                         "rep_paused={} mbg={} num_props={} cmd_name={} "
                         "props_added={} test_mode='{}'",
                         pkt_seq, pkt_len, bunch_bits, target_desc,
                         config_.v3_reliable, effective_rp, config_.v3_has_mbg,
                         config_.v3_num_properties, config_.v3_cmd_handle_name,
                         props_added, config_.v3_test_mode);
            return true;
        }
        spdlog::warn("[Replay][V3] send_to_client failed");
        return false;
    }

    void send_keepalive(ClientState& cs, const sockaddr_in& client_addr,
                        const std::string& key) {
        uint8_t buf[128] = {};
        size_t off = 0;

        // ── Packet prefix (outer header + notify + history + custom field)
        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);

        // ── PacketInfo ─────────────────────────────────────────────────
        // AoC client ALWAYS expects bHasPacketInfoPayload=1. Sending 0 causes
        // the handler to misprocess the packet → ZeroLastByte disconnect.
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);  // bHasPacketInfoPayload
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10); // JitterClockTimeMS
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);  // bHasServerFrameTime=0
// CoA captures, C | Y | B code

        // ── Termination ────────────────────────────────────────────────
        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // extra sentinel bit
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);

        uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        int sent = send_to_client(buf, pkt_len, client_addr);
        if (sent > 0) {
            spdlog::debug("[GameServer] >> KeepAlive to {} ({}B seq={} ack={} custom={})",

                          key, pkt_len, sent_seq, cs.in_ack_seq,
                          cs.custom_field_captured ? "yes" : "no");
            spdlog::debug("[GameServer]    hex: {}", ue5::hex_dump(buf, pkt_len, 64));
        } else {
            spdlog::warn("[GameServer]    KeepAlive send FAILED");
        }

        ProxyLogger::instance().log_udp("S>C", key, buf, pkt_len, "KeepAlive");
    }

    /// Send ClientAckGoodMove (S>C, ch=19, unreliable, 68-bit payload) to
    /// acknowledge a ServerMove from the client.  This clears the client's
    /// saved move buffer for the acknowledged timestamp, stopping prediction
    /// correction artefacts (rubber-banding / movement stutter).
    ///
    /// Wire format (all fields LSB-first within each byte, UE5 style):
    /// Send ClientAckGoodMove (S>C, ch=19, unreliable, 68-bit bunch payload) to
    /// acknowledge client's ServerMove with authoritative ContentBlock framing.
    ///
    /// Framing per working MITM capture contract (291 verified instances):
    ///   Bunch Header:
    ///     bControl(1)=0, bIsRepPaused(1)=0, bReliable(1)=0
    ///     ChIndex(8)=SIP(ch_idx) (0x26 for ch=19)
    ///     bExports(1)=0, bGuids(1)=0, bPartial(1)=0
    ///     BunchDataBits(13)=68
    ///   Bunch Data (68 bits):
    ///     ContentBlock: bHasRepLayout(1)=0, bIsActor(1)=1, NumPayloadBits=58 (SIP 8b, 0x74)
    ///     Field Header: RepIndex(8)=35 (ClientAckGoodMove), NumFieldPayloadBits=42 (SIP 8b, 0x54)
    ///     RPC Prefix: 10 bits = 645 (0b1010000101)
    ///     Parameter: TimeStamp(32)=IEEE 754 float
    void send_client_ack_good_move(ClientState& cs, const sockaddr_in& client_addr,
                                   const std::string& key, float server_move_ts,
                                   uint32_t ch_idx = 19) {
        uint8_t buf[128] = {};
        size_t off = 0;

        // ── Packet prefix (outer header + notify + history + custom field)
        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);

        // ── PacketInfo ─────────────────────────────────────────────────
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);  // bHasPacketInfoPayload=1
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10); // JitterClockTimeMS=1023
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);  // bHasServerFrameTime=0

        // ── S>C bunch header: ch=ch_idx, unreliable ────────────────────
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bControl=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bIsReplicationPaused=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bReliable=0
        ue5::write_sip(buf, sizeof(buf), off, ch_idx);
        ue5::write_bits(buf, sizeof(buf), off, 0,    1); // bExports=0
        ue5::write_bits(buf, sizeof(buf), off, 0,    1); // bGuids=0
        ue5::write_bits(buf, sizeof(buf), off, 0,    1); // bPartial=0
        // BunchDataBits: 13 bits — 68 bits verified against working capture
        ue5::write_bits(buf, sizeof(buf), off, 68,  13); // BunchDataBits=68

        // ── ClientAckGoodMove bunch data (68 bits) ─────────────────────
        // ContentBlock header (10 bits)
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bHasRepLayout=0 (RPC)
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);  // bIsActor=1
        ue5::write_sip(buf, sizeof(buf), off, 58);     // NumPayloadBits=58

        // Field header (16 bits)
        ue5::write_bits(buf, sizeof(buf), off, 35, 8); // RepIndex=35 (ClientAckGoodMove wire pos)
        ue5::write_sip(buf, sizeof(buf), off, 42);     // NumFieldPayloadBits=42

        // RPC prefix: 10 bits = 645 (0b1010000101)
        ue5::write_bits(buf, sizeof(buf), off, 645, 10);

        // Parameter: float TimeStamp (32 bits)
        uint32_t ts_bits = 0;
        std::memcpy(&ts_bits, &server_move_ts, sizeof(float));
        ue5::write_bits(buf, sizeof(buf), off, ts_bits, 32);

        // ── Termination ────────────────────────────────────────────────
        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // sentinel
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);

        uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        int sent = send_to_client(buf, pkt_len, client_addr);
        if (sent > 0) {
            spdlog::info("[S>C] >> ClientAckGoodMove to {} ({}B seq={} ack={} ts={:.4f}s)",
                         key, pkt_len, sent_seq, cs.in_ack_seq, server_move_ts);
        } else {
            spdlog::warn("[GameServer]    ClientAckGoodMove send FAILED");
        }

        ProxyLogger::instance().log_udp("S>C", key, buf, pkt_len, "ClientAckGoodMove");
    }

    /// NPC Dialogue RPC: ClientSetDialogue
    /// Exec thunk in AOCClient-Win64-Shipping.exe @ 0x145d32650:
    ///   Param 0: Response (TArray<FRecordGuid>) -> ArrayNum via SIP, then 64-bit GUIDs
    ///   Param 1: Choices (TArray<FRecordGuid>)  -> ArrayNum via SIP, then 64-bit GUIDs
    ///   Param 2: DialogSourceActor (AActor*)   -> 1 bit b_has_guid + 128-bit NetGUID
    ///   Param 3: CalleResponseRecord (FRecordGuid) -> 64-bit raw
    ///   Param 4: VoiceGuidOverride (FRecordGuid)   -> 64-bit raw
    /// Calls virtual AAoCPlayerController::ClientSetDialogue_Implementation at vtable[+0x16e0].
    /// Flags: 0x01020cc0 (FUNC_NetClient | FUNC_NetReliable | FUNC_Net).
    /// Target Channel: Channel 3 (AAoCPlayerController root actor).
    void send_client_set_dialogue(
        ClientState& cs, const sockaddr_in& client_addr,
        const std::string& key,
        uint32_t npc_guid_lo, uint32_t npc_guid_hi,
        uint32_t npc_srv, uint32_t npc_rnd,
        uint64_t response_guid = 0x5429e64f47010001ULL,
        uint64_t choice_guid = 0x5429e4fb579b00dfULL,
        uint64_t calle_response_rec = 0ULL,
        uint64_t voice_guid_override = 0ULL) {

        uint8_t buf[1024] = {};
        size_t off = 0;

        // 1. Packet prefix (outer header + notify + history + custom field)
        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);

        // 2. PacketInfo
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);  // bHasPacketInfoPayload=1
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10); // JitterClockTimeMS=1023
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);  // bHasServerFrameTime=0

        // 3. S>C bunch header: ch=3 (AAoCPlayerController), RELIABLE
        constexpr uint32_t kPcChannel = 3;
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bControl=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bIsReplicationPaused=0
        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // bReliable=1
        ue5::write_sip(buf, sizeof(buf), off, kPcChannel); // ChIndex=3 (0x06)
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bExports=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bGuids=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bPartial=0

        // ChSequence (10 bits) on Channel 3
        uint16_t ch_seq = 1;
        auto trk_it = cs.last_outgoing_reliable_chseq.find(kPcChannel);
        if (trk_it != cs.last_outgoing_reliable_chseq.end()) {
            ch_seq = (trk_it->second + 1) & 0x3FF;
            if (ch_seq == 0) ch_seq = 1;
            trk_it->second = ch_seq;
        } else {
            ch_seq = (cs.reliable_seq + 1) & 0x3FF;
            if (ch_seq == 0) ch_seq = 1;
            cs.last_outgoing_reliable_chseq[kPcChannel] = ch_seq;
        }
        ue5::write_bits(buf, sizeof(buf), off, ch_seq, 10);
// -- C O A x C_Y_B --

        // ChName: bIsHardcoded=1, EName=102 (NAME_Actor)
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);
        ue5::write_sip(buf, sizeof(buf), off, 102);

        // Record offset where BunchDataBits (13 bits) will be written
        size_t bunch_data_bits_off = off;
        ue5::write_bits(buf, sizeof(buf), off, 0, 13); // placeholder
        size_t bunch_start_off = off;

        // 4. ContentBlock header:
        // Targeting channel root actor (AAoCPlayerController)
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bHasRepLayout=0 (RPC)
        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // bIsActor=1

        auto read_probe_fn = [](const char* path, uint32_t default_val) -> uint32_t {
            std::FILE* fp = std::fopen(path, "r");
            if (!fp) return default_val;
            uint32_t n = default_val;
            std::fscanf(fp, "%u", &n);
            std::fclose(fp);
            return n;
        };

        auto read_probe_u64 = [](const char* path, uint64_t default_val) -> uint64_t {
            std::FILE* fp = std::fopen(path, "r");
            if (!fp) return default_val;
            uint64_t n = default_val;
            if (std::fscanf(fp, "0x%llx", &n) != 1) {
                std::rewind(fp);
                std::fscanf(fp, "%llu", &n);
            }
            std::fclose(fp);
            return n;
        };

        // Probe overrides if present:
        uint32_t wire_handle = read_probe_fn("probe_dialogue_handle.txt", 258u);
        uint32_t handle_max  = read_probe_fn("probe_dialogue_max.txt", 1025u);
        response_guid        = read_probe_u64("probe_dialogue_response.txt", response_guid);
        choice_guid          = read_probe_u64("probe_dialogue_choice.txt", choice_guid);

        // Build parameters into param_buf:
        uint8_t param_buf[512] = {};
        size_t param_off = 0;

        // Param 0: Response (TArray<FRecordGuid>) -> ArrayNum via SIP, then 64-bit GUIDs
        ue5::write_sip(param_buf, sizeof(param_buf), param_off, 1);
        ue5::write_bits(param_buf, sizeof(param_buf), param_off, response_guid, 64);

        // Param 1: Choices (TArray<FRecordGuid>) -> ArrayNum via SIP, then 64-bit GUIDs
        ue5::write_sip(param_buf, sizeof(param_buf), param_off, 1);
        ue5::write_bits(param_buf, sizeof(param_buf), param_off, choice_guid, 64);

        // Param 2: DialogSourceActor (AActor*) -> 1 bit b_has_guid + 128-bit NetGUID
        ue5::write_bits(param_buf, sizeof(param_buf), param_off, 1, 1); // b_has_guid=1
        ue5::write_bits(param_buf, sizeof(param_buf), param_off, npc_guid_lo, 32);
        ue5::write_bits(param_buf, sizeof(param_buf), param_off, npc_guid_hi, 32);
        ue5::write_bits(param_buf, sizeof(param_buf), param_off, npc_srv, 32);
        ue5::write_bits(param_buf, sizeof(param_buf), param_off, npc_rnd, 32);

        // Param 3: CalleResponseRecord (FRecordGuid) -> 64-bit raw
        ue5::write_bits(param_buf, sizeof(param_buf), param_off, calle_response_rec, 64);

        // Param 4: VoiceGuidOverride (FRecordGuid) -> 64-bit raw
        ue5::write_bits(param_buf, sizeof(param_buf), param_off, voice_guid_override, 64);

        size_t param_bits = param_off;

        // Build field header (FieldNetIndex + SIP(param_bits)) into field_buf:
        uint8_t field_buf[512] = {};
        size_t field_off = 0;
        ue5::write_serialize_int(field_buf, sizeof(field_buf), field_off, wire_handle, handle_max);
        ue5::write_sip(field_buf, sizeof(field_buf), field_off, param_bits);

        // Append param_buf to field_buf
        for (size_t i = 0; i < param_bits; ++i) {
            uint64_t bit = ue5::read_bits_at(param_buf, sizeof(param_buf), i, 1);
            ue5::write_bits(field_buf, sizeof(field_buf), field_off, bit, 1);
        }
        size_t cb_payload_bits = field_off;

        // Write cb_payload_bits as SIP into buf
        ue5::write_sip(buf, sizeof(buf), off, cb_payload_bits);

        // Write field_buf into buf
        for (size_t i = 0; i < cb_payload_bits; ++i) {
            uint64_t bit = ue5::read_bits_at(field_buf, sizeof(field_buf), i, 1);
            ue5::write_bits(buf, sizeof(buf), off, bit, 1);
        }

        // Patch BunchDataBits
        size_t bunch_data_bits = off - bunch_start_off;
        ue5::patch_bits(buf, sizeof(buf), bunch_data_bits_off, bunch_data_bits, 13);

        // Termination
        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // sentinel
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);

        uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        int sent = send_to_client(buf, pkt_len, client_addr);
        if (sent > 0) {
            spdlog::info("[S>C] >> ★ ClientSetDialogue to {} ({}B seq={} ack={} chSeq={} handle={} npc=0x{:x} resp=0x{:016x} choice=0x{:016x})",
                         key, pkt_len, sent_seq, cs.in_ack_seq, ch_seq, wire_handle,
                         npc_guid_lo, response_guid, choice_guid);
        } else {
            spdlog::warn("[GameServer]    ClientSetDialogue send FAILED");
        }

        ProxyLogger::instance().log_udp("S>C", key, buf, pkt_len, "ClientSetDialogue");
    }

    /// Decodes ServerUpdateLevelVisibility (PC field 92).  With
    /// --send-level-visibility-acks, requests that make a level visible are
    /// acked; mid-replay they are queued and drained when the replay ends,
    /// because a native ch=3 bunch inside a replayed partial chain would
    /// interrupt it.
    void handle_server_update_level_visibility(
            const uint8_t* data, size_t data_len,
            const aoc::protocol::wire::FActorRpcBunch& rpc, size_t bunch_end,
            uint32_t ch_idx, uint16_t ch_seq,
            const std::string* client_key, const sockaddr_in* client_addr) {
        aoc::protocol::wire::LevelVisibilityRequest req;
        if (!aoc::protocol::wire::parse_server_update_level_visibility(
                data, data_len, rpc.param_bit_offset, bunch_end, req)) {
            spdlog::warn("[C>S-RPC] ch={} chSeq={} >> SERVER UPDATE LEVEL VISIBILITY: "
                         "params did not decode ({} bits)", ch_idx, ch_seq, rpc.param_bit_count);
            return;
        }
        spdlog::info("[C>S-RPC] ch={} chSeq={} >> SERVER UPDATE LEVEL VISIBILITY: "
                     "visible={} flag3={} file_differs={} txn_bits={} pkg='{}'",
                     ch_idx, ch_seq, req.is_visible, req.flag3, req.has_file_name,
                     req.transaction_bits.size(), req.package_name);

        if (!config_.send_level_visibility_acks || !client_key || !client_addr) return;
        if (!req.is_visible && !req.flag3) return;  // hiding a level needs no permission

        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(*client_key);
        if (it == clients_.end() || it->second.phase < ClientState::CONNECTED) return;
        ClientState& cs = it->second;
        if (replay_active_.load()) {
            cs.pending_sulv_acks.push_back(ClientState::PendingSulvAck{ std::move(req), ch_idx });
            spdlog::info("[C>S-RPC]   ack queued until replay completes (depth {})",
                         cs.pending_sulv_acks.size());
            return;
        }
        (void)send_client_ack_update_level_visibility(cs, *client_addr, *client_key, req, ch_idx);
    }

    /// Possession handshake from the client (PC fields 73/77/78; see
    /// protocol/wire/possession.h).  ServerCheckClientPossession means the
    /// client has no acknowledged pawn; the reliable variant is what
    /// ClientRestart_Implementation sends when the pawn in ClientRestart did
    /// not resolve.  ServerAcknowledgePossession means ClientRestart worked.
    /// With --answer-possession-checks, a check is answered with ClientRestart,
    /// held back while the replay runs (a native ch=3 bunch inside a replayed
    /// partial chain would interrupt it).
    void handle_possession_rpc(const uint8_t* data, size_t data_len,
                               const aoc::protocol::wire::FActorRpcBunch& rpc, size_t bunch_end,
                               uint32_t ch_idx, uint16_t ch_seq,
                               const std::string* client_key, const sockaddr_in* client_addr) {
        namespace wire = aoc::protocol::wire;
        if (rpc.fn_idx == wire::kPcFieldIdx_ServerAcknowledgePossession) {
            aoc::protocol::emit::FIntrepidNetworkGUID pawn;
            if (wire::parse_pawn_param(data, data_len, rpc.param_bit_offset, bunch_end, pawn))
                spdlog::warn("[C>S-RPC] ch={} chSeq={} >> SERVER ACKNOWLEDGE POSSESSION: pawn={} "
                             "srv={} rnd=0x{:08x} (the client possessed its pawn)",
                             ch_idx, ch_seq, pawn.ObjectId, pawn.ServerId, pawn.Randomizer);
            else
                spdlog::warn("[C>S-RPC] ch={} chSeq={} >> SERVER ACKNOWLEDGE POSSESSION: "
                             "params did not decode ({} bits)", ch_idx, ch_seq, rpc.param_bit_count);
            return;
        }
// ~ C:Y:B - C-O-A ~

        const bool reliable = rpc.fn_idx == wire::kPcFieldIdx_ServerCheckClientPossessionReliable;
        spdlog::info("[C>S-RPC] ch={} chSeq={} >> SERVER CHECK CLIENT POSSESSION{}",
                     ch_idx, ch_seq, reliable ? " RELIABLE (the ClientRestart pawn did not resolve)" : "");
        if (!config_.answer_possession_checks || !client_key || !client_addr) return;

        std::lock_guard<std::mutex> lk(client_mu_);
        auto it = clients_.find(*client_key);
        if (it == clients_.end() || it->second.phase < ClientState::CONNECTED) return;
        ClientState& cs = it->second;
        if (replay_active_.load()) {
            cs.client_restart_pending = true;
            spdlog::info("[C>S-RPC]   ClientRestart queued until replay completes");
            return;
        }
        (void)send_client_restart(cs, *client_addr, *client_key);
    }

    /// The player pawn the captured session spawned, as the original server
    /// named it in ClientRestart (replay_data.bin pkt#97): ObjectId 10341538,
    /// minted by server 60, whose Randomizer 0x6ee87ee4 is shared by every
    /// GUID it minted in the capture.  ("88" in older notes is the short
    /// export form inside the pawn open, not the GUID.)
    static aoc::protocol::emit::FIntrepidNetworkGUID replay_pawn_guid() {
        aoc::protocol::emit::FIntrepidNetworkGUID g;
        g.ObjectId   = 10341538;
        g.ServerId   = 60;
        g.Randomizer = 0x6ee87ee4;
        return g;
    }

    /// Sends APlayerController::ClientRestart(Pawn) (PC field 45); encoding in
    /// protocol/wire/possession.h, bit-identical to the original server's
    /// (libs/aocnet/tests/test_possession.cpp).  At most three per session,
    /// so a pawn the client cannot resolve (it answers
    /// ServerCheckClientPossessionReliable) does not loop.  Replay mode only:
    /// native mode spawns its own pawn, whose GUID is not wired up here yet.
    /// Caller must hold client_mu_.
    bool send_client_restart(ClientState& cs, const sockaddr_in& client_addr, const std::string& key) {
        constexpr int kMaxClientRestarts = 3;
        if (cs.client_restarts_sent >= kMaxClientRestarts) {
            spdlog::warn("[S>C] ClientRestart not sent: already sent {} this session",
                         cs.client_restarts_sent);
            return false;
        }
        if (config_.native_mode || !replay_data_) {
            spdlog::warn("[S>C] ClientRestart not sent: only the replayed pawn is known");
            return false;
        }
        const aoc::protocol::emit::FIntrepidNetworkGUID pawn = replay_pawn_guid();

        uint8_t payload[64] = {};
        size_t payload_bits = 0;
        aoc::protocol::wire::write_pc_object_rpc(payload, sizeof(payload), payload_bits,
                                                  aoc::protocol::wire::kPcFieldIdx_ClientRestart, pawn);
        if (!send_pc_reliable_rpc(cs, client_addr, key, payload, sizeof(payload), payload_bits,
                                  "ClientRestart"))
            return false;
        ++cs.client_restarts_sent;
        spdlog::warn("[S>C]    ClientRestart pawn={} srv={} rnd=0x{:08x} ({} of {} this session)",
                     pawn.ObjectId, pawn.ServerId, pawn.Randomizer,
                     cs.client_restarts_sent, kMaxClientRestarts);
        return true;
    }

    /// Sends APlayerController::ClientAckUpdateLevelVisibility (PC field 21)
    /// on the PC channel in answer to a decoded ServerUpdateLevelVisibility.
    /// Payload encoding and field index: protocol/wire/level_visibility.h
    /// (tested by tools/test_level_visibility.cpp).
    ///
    /// Captured requests never set the third flag, and levels became visible
    /// in every run without any ack, so this is opt-in
    /// (--send-level-visibility-acks) for live experiments.
    ///
    /// The previous stub (dispatch byte 0x0E, raw 32-bit TransactionId, no
    /// content-block size, no send bits) was malformed; see git history.
    ///
    /// Requires a ch=3 entry in cs.last_outgoing_reliable_chseq (fed by
    /// patch_replay_chseq / scan_outgoing_packet_chseq).  Without one the
    /// correct chSeq is unknown and a guess would stall or close ch=3, so we
    /// skip.  Caller must hold client_mu_.
    bool send_client_ack_update_level_visibility(
            ClientState& cs, const sockaddr_in& client_addr,
            const std::string& key,
            const aoc::protocol::wire::LevelVisibilityRequest& request,
            uint32_t triggering_ch_idx) {
        uint8_t payload[512] = {};
        size_t payload_bits = 0;
        aoc::protocol::wire::write_client_ack_update_level_visibility(
            payload, sizeof(payload), payload_bits, request, /*can_make_visible=*/true);
        if (!send_pc_reliable_rpc(cs, client_addr, key, payload, sizeof(payload), payload_bits,
                                  "ClientAckUpdateLevelVisibility"))
            return false;
        spdlog::info("[S>C]    ClientAckUpdateLevelVisibility txn_bits={} triggered_by_ch={} pkg='{}'",
                     request.transaction_bits.size(), triggering_ch_idx, request.package_name);
        return true;
    }
// ~ Cyb & C-O-A ~

    /// Sends AAoCPlayerController::ClientHitConfirmationsReliable (PC field
    /// 171) with one SerializedData blob per confirmation; encoding in
    /// protocol/wire/hit_confirmation.h (libs/aocnet/tests/test_hit_confirmation.cpp).
    /// Opt-in via --send-hit-confirmations.  Caller must hold client_mu_.
    bool send_client_hit_confirmations(
            ClientState& cs, const sockaddr_in& client_addr, const std::string& key,
            const std::vector<aoc::protocol::wire::HitConfirmation>& hits) {
        std::vector<std::vector<uint8_t>> blobs;
        for (const auto& hc : hits)
            blobs.push_back(aoc::protocol::wire::serialize_hit_confirmation(hc));
        uint8_t payload[1024] = {};
        size_t payload_bits = 0;
        aoc::protocol::wire::write_client_hit_confirmations(
            payload, sizeof(payload), payload_bits, blobs, /*reliable=*/true);
        if (!send_pc_reliable_rpc(cs, client_addr, key, payload, sizeof(payload), payload_bits,
                                  "ClientHitConfirmationsReliable"))
            return false;
        for (const auto& hc : hits)
            spdlog::info("[S>C]    ClientHitConfirmationsReliable inst=0x{:x} record=0x{:x} "
                         "source_record=0x{:x} instigator={}",
                         hc.inst_guid, hc.record_guid, hc.source_record_guid,
                         hc.instigator.object_lo);
        return true;
    }

    /// Frames `payload` (a complete actor content block) as one reliable
    /// bunch on the PC channel (ch=3) and sends it.  The chSeq is the ch=3
    /// tracker + 1 (fed by patch_replay_chseq / scan_outgoing_packet_chseq);
    /// with no tracker entry the correct chSeq is unknown and a guess would
    /// stall or close ch=3, so we skip.  Caller must hold client_mu_.
    bool send_pc_reliable_rpc(ClientState& cs, const sockaddr_in& client_addr,
                              const std::string& key, const uint8_t* payload,
                              size_t payload_cap, size_t payload_bits, const char* label) {
        constexpr uint32_t kPcChannel = 3;
        auto trk_it = cs.last_outgoing_reliable_chseq.find(kPcChannel);
        if (trk_it == cs.last_outgoing_reliable_chseq.end()) {
            spdlog::warn("[S>C] {} skipped: no ch=3 chSeq tracker entry yet", label);
            return false;
        }
        if (payload_bits >= (1u << 13) || payload_bits > payload_cap * 8) {
            spdlog::warn("[S>C] {} skipped: payload {} bits does not fit one bunch", label, payload_bits);
            return false;
        }

        uint8_t buf[1500] = {};
        size_t off = 0;
        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);

        // PacketInfo — same as the other native S>C senders.
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);  // bHasPacketInfoPayload
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10); // JitterClockTimeMS
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);  // bHasServerFrameTime

        // Bunch header: reliable, PC channel.
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bControl
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bIsReplicationPaused
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);  // bReliable
        ue5::write_sip (buf, sizeof(buf), off, kPcChannel);
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bHasPackageMapExports
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bHasMustBeMappedGUIDs
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bPartial

        uint16_t ch_seq = (trk_it->second + 1) & 0x3FF;  // 10-bit ChSequence
        if (ch_seq == 0) ch_seq = 1;
        ue5::write_bits(buf, sizeof(buf), off, ch_seq, 10);

        ue5::write_bits(buf, sizeof(buf), off, 1, 1);    // ChName.bIsHardcoded
        ue5::write_sip (buf, sizeof(buf), off, 102);     // NAME_Actor
        aoc::protocol::wire::write_bunch_data_bits(buf, sizeof(buf), off,
                                                   static_cast<uint32_t>(payload_bits));  // BunchDataBits

        size_t r = 0;
        for (size_t i = 0; i < payload_bits; ++i)
            ue5::write_bits(buf, sizeof(buf), off,
                            ue5::read_bits(payload, payload_cap, r, 1), 1);

        ue5::write_bits(buf, sizeof(buf), off, 1, 1);    // AoC sentinel bit
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);

        uint16_t sent_seq = cs.out_seq;
        int sent = send_to_client(buf, pkt_len, client_addr);
        if (sent <= 0) {
            spdlog::warn("[S>C] {} send FAILED", label);
            return false;
        }
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;
        trk_it->second = ch_seq;

        spdlog::info("[S>C] >> {} to {} ({}B seq={} chSeq={} bits={})",
                     label, key, pkt_len, sent_seq, ch_seq, payload_bits);
        ProxyLogger::instance().log_udp("S>C", key, buf, pkt_len, label);
        return true;
    }

    /// Phase D2: Send a single generator-produced bunch to the client.
    /// Framing mirrors send_client_ack_good_move (the minimal S>C data-
    /// packet path we already trust):
    ///   outer header + FNetPacketNotify + AoC custom field + PacketInfo
    ///   + bunch header + payload + sentinel + byte termination.
    ///
    /// D2 scope assumes unreliable, non-control, non-partial bunches only
    /// (matches RecordedBunchGenerator).  Reliable / control / partial are
    /// rejected with a warning so we don't put malformed bytes on the wire.
    ///
    /// Returns true on successful sendto(); false on any validation or I/O
    /// failure.  The caller must hold no locks beyond the ClientState lock
    /// if any — this mirrors send_keepalive / send_client_ack_good_move.
    bool send_generated_bunch(ClientState& cs, const sockaddr_in& client_addr,
                              const std::string& key,
                              const aoc::GeneratedBunch& gb,
                              std::string_view gen_name) {
        // ── D2 invariants: reject anything we haven't wired up yet ─────
        if (gb.b_control || gb.b_reliable || gb.b_partial ||
            gb.b_has_exports || gb.b_has_must_map) {
            spdlog::warn("[GEN] ch={} '{}' bunch rejected: D2 only supports "
                         "unreliable/non-control/non-partial "
                         "(ctrl={} rel={} part={} exp={} mm={})",
                         gb.channel, gen_name, gb.b_control, gb.b_reliable,
                         gb.b_partial, gb.b_has_exports, gb.b_has_must_map);
            return false;
        }
        if (gb.bit_count == 0 || gb.bit_count > (1u << 14) - 1u) {
            spdlog::warn("[GEN] ch={} '{}' bunch rejected: bit_count={} "
                         "out of 14-bit range", gb.channel, gen_name, gb.bit_count);
            return false;
        }
        const size_t payload_bytes_needed = (gb.bit_count + 7) / 8;
        if (gb.payload.size() < payload_bytes_needed) {
            spdlog::warn("[GEN] ch={} '{}' bunch rejected: payload.size()={} "
                         "< needed={} for bit_count={}",
                         gb.channel, gen_name,
                         gb.payload.size(), payload_bytes_needed, gb.bit_count);
            return false;
        }
// ~ C O A :: C:Y:B ~

        uint8_t buf[512] = {};
        size_t off = 0;

        // ── Packet prefix (outer header + notify + history + custom field)
        write_sc_packet_prefix(buf, sizeof(buf), off, cs, /*handshake=*/false);

        // ── PacketInfo ─────────────────────────────────────────────────
        ue5::write_bits(buf, sizeof(buf), off, 1,    1);  // bHasPacketInfoPayload=1
        ue5::write_bits(buf, sizeof(buf), off, 1023, 10); // JitterClockTimeMS=1023
        ue5::write_bits(buf, sizeof(buf), off, 0,    1);  // bHasServerFrameTime=0

        // ── Bunch header: unreliable, non-control ──────────────────────
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bControl=0
        ue5::write_bits(buf, sizeof(buf), off, gb.b_is_replication_paused ? 1 : 0, 1);
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bReliable=0

        // ChIndex (SerializeIntPacked)
        ue5::write_sip(buf, sizeof(buf), off, gb.channel);

        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bHasExports=0
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bHasMustMap=0 (a.k.a. bGuids)
        ue5::write_bits(buf, sizeof(buf), off, 0, 1); // bPartial=0
        // BunchDataBits — 13 bits (canonical spec, top of file).
        aoc::protocol::wire::write_bunch_data_bits(buf, sizeof(buf), off,
                                                   static_cast<uint32_t>(gb.bit_count)); // BunchDataBits (S>C)

        // ── Payload: copy bit_count bits LSB-first ─────────────────────
        // The generator stores bytes in the same byte-order the wire uses
        // (first_8_bytes column of blueprint_bunches.csv).  Each source byte
        // is LSB-first in the packed bitstream, so copy byte-by-byte for
        // full bytes, then fractional bits via a single write_bits call.
        uint32_t full_bytes   = gb.bit_count / 8;
        uint32_t tail_bits    = gb.bit_count - full_bytes * 8;
        for (uint32_t i = 0; i < full_bytes; ++i) {
            ue5::write_bits(buf, sizeof(buf), off, gb.payload[i], 8);
        }
        if (tail_bits > 0) {
            uint32_t tail = gb.payload[full_bytes] & ((1u << tail_bits) - 1u);
            ue5::write_bits(buf, sizeof(buf), off, tail, tail_bits);
        }

        // ── Termination ────────────────────────────────────────────────
        ue5::write_bits(buf, sizeof(buf), off, 1, 1); // sentinel
        size_t pkt_len = ue5::add_termination(buf, sizeof(buf), off);

        uint16_t sent_seq = cs.out_seq;
        cs.out_seq = (cs.out_seq + 1) & ClientState::SEQ_MASK;

        int sent = send_to_client(buf, pkt_len, client_addr);
        if (sent > 0) {
            spdlog::info("[GEN] >> sent ch={} gen='{}' ({}B seq={} ack={} bits={})",
                         gb.channel, gen_name, pkt_len, sent_seq,
                         cs.in_ack_seq, gb.bit_count);
            ProxyLogger::instance().log_udp("S>C", key, buf, pkt_len, "GeneratedBunch");
            return true;
        }
        spdlog::warn("[GEN] ch={} '{}' send FAILED", gb.channel, gen_name);
        return false;
    }

    /// Send periodic keepalives to all connected/handshake-complete clients.
    /// Called from run_loop() on recv timeout (~every 100ms).
    void send_periodic_keepalives() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(client_mu_);
        for (auto& [key, cs] : clients_) {
            // Only send to clients that have completed at least the handshake
            if (cs.phase < ClientState::HANDSHAKE_COMPLETE) continue;

            // Determine keepalive interval based on NMT state
            // After NMT_Join (nmt_state >= 4): every 200ms (aggressive)
            // During NMT handshake: every 500ms
            auto interval = (cs.nmt_state >= 4)
                ? std::chrono::milliseconds(200)
                : std::chrono::milliseconds(500);

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - cs.last_keepalive_sent);
            if (elapsed < interval) continue;

            // Check for connection timeout.
            // During bootstrap/replay the game goes into a loading screen and
            // stops sending C>S UDP packets for 2-3 minutes; use a generous
            // limit so we don't kill the client mid-load. Normal idle timeout
            // stays at 30s.
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                now - cs.last_activity);
            bool active_send = (cs.bootstrap_active || replay_active_.load()) &&
                               cs.phase == ClientState::CONNECTED;
            // Use 300s when replay/bootstrap is active (post-GameSpecific loading).
            // Use 120s when NMT_Welcome was already sent (nmt_state>=2) but the
            // client hasn't sent GameSpecific yet — Verra_World_Master can take
            // 60-90s to load before the client is ready to fire NMT_GameSpecific.
            // Use 30s for everything else (idle/stale connections).
            int timeout_sec = active_send ? 300
                            : (cs.nmt_state >= 2) ? 120
                            : 30;
            if (idle.count() > timeout_sec) {
                spdlog::info("[GameServer] Client {} timed out ({}s idle, limit={}s), removing",
                             key, idle.count(), timeout_sec);
                cs.phase = ClientState::AWAITING_INITIAL;
                continue;
            }

            // Skip periodic keepalives while bootstrap_loop / replay_loop is
            // running — those loops send their own keepalives, and advancing
            // out_seq from two threads simultaneously causes sequence gaps
            // that confuse the client.
            if (active_send)
                continue;

            send_keepalive(cs, cs.addr, key);
            cs.last_keepalive_sent = now;
        }
    }

    // ── Client-facing receive loop ──────────────────────────────────────

    void run_loop() {
        uint8_t buf[65536];
        sockaddr_in client_addr{};

        spdlog::info("[GameServer] Receive loop started ({})",
                     is_relay_mode() ? "RELAY" : "EMULATION");

        while (running_) {
#ifdef _WIN32
            int addr_len = sizeof(client_addr);
            int recv_len = recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                    reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
#else
            socklen_t addr_len = sizeof(client_addr);
            int recv_len = recvfrom(sock_, buf, sizeof(buf), 0,
                                    reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
#endif
            if (recv_len <= 0) {
                // On recv timeout, send keepalives to connected clients
                if (!is_relay_mode()) {
                    send_periodic_keepalives();
                }
                continue;
            }
// C | Y | B + CoA

            std::string remote = addr_key(client_addr);
            uint64_t seq = ++pkt_c2s_;

            spdlog::info("[GameServer] << C>S #{} from {} {}B", seq, remote, recv_len);
            spdlog::debug("[GameServer]    hex: {}", ue5::hex_dump(buf, recv_len, 512));

            ProxyLogger::instance().log_udp("C>S", remote, buf, recv_len, "");

            if (relay_active_.load() || is_relay_mode()) {
                // ── RELAY MODE ──────────────────────────────────────
                // Also log as handshake parse for analysis
                ue5_hs::HandshakePacket hpkt;
                if (ue5_hs::parse(buf, static_cast<size_t>(recv_len), hpkt) && hpkt.is_handshake) {
                    spdlog::info("[GameServer] RELAY C>S Handshake type={} ({}) count={} netVer=0x{:08x}",
                                 hpkt.packet_type,
                                 hpkt.packet_type == 0 ? "Initial" :
                                 hpkt.packet_type == 2 ? "Response" :
                                 hpkt.packet_type == 3 ? "Ack" : "?",
                                 hpkt.sent_count, hpkt.network_version);
                    if (hpkt.has_custom_ext)
                        spdlog::info("[GameServer] RELAY   CharID={} Token={}", hpkt.char_id, hpkt.token);
                } else if (!hpkt.is_handshake) {
                    spdlog::info("[GameServer] RELAY C>S Data {}B", recv_len);
                }
                {
                    std::lock_guard<std::mutex> lk(client_mu_);
                    last_client_addr_ = client_addr;
                    client_known_ = true;
                }
                int sent = sendto(upstream_sock_,
                                  reinterpret_cast<const char*>(buf),
                                  recv_len, 0,
                                  reinterpret_cast<sockaddr*>(&upstream_addr_),
                                  sizeof(upstream_addr_));
                if (sent <= 0) {
                    spdlog::warn("[GameServer] Forward FAILED");
                } else {
                    spdlog::info("[GameServer]    -> forwarded {}B upstream", sent);
                }
            } else {
                // ── EMULATION MODE ──────────────────────────────────
                handle_packet(buf, static_cast<size_t>(recv_len), client_addr);
            }
        }
        spdlog::info("[GameServer] Client loop ended");
    }

    // ── Upstream receive loop (relay mode only) ─────────────────────────

    void upstream_loop() {
        uint8_t buf[65536];
        sockaddr_in from_addr{};

        spdlog::info("[GameServer] Upstream loop started");

        while (running_) {
#ifdef _WIN32
            int addr_len = sizeof(from_addr);
            int recv_len = recvfrom(upstream_sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                    reinterpret_cast<sockaddr*>(&from_addr), &addr_len);
#else
            socklen_t addr_len = sizeof(from_addr);
            int recv_len = recvfrom(upstream_sock_, buf, sizeof(buf), 0,
                                    reinterpret_cast<sockaddr*>(&from_addr), &addr_len);
#endif
            if (recv_len <= 0) continue;

            std::string remote = addr_key(from_addr);
            uint64_t seq = ++pkt_s2c_;

            spdlog::info("[GameServer] << S>C #{} from {} {}B", seq, remote, recv_len);
            spdlog::debug("[GameServer]    hex: {}", ue5::hex_dump(buf, recv_len, 512));

            // Parse handshake for logging
            ue5_hs::HandshakePacket hpkt;
            if (ue5_hs::parse(buf, static_cast<size_t>(recv_len), hpkt) && hpkt.is_handshake) {
                spdlog::info("[GameServer] RELAY S>C Handshake type={} ({}) count={} ts={:.6f}",
                             hpkt.packet_type,
                             hpkt.packet_type == 1 ? "Challenge" :
                             hpkt.packet_type == 3 ? "Ack" :
                             hpkt.packet_type == 4 ? "Restart" : "?",
                             hpkt.sent_count, hpkt.timestamp);
                spdlog::info("[GameServer] RELAY   cookie: {}",
                             ue5::hex_dump(hpkt.cookie, ue5_hs::COOKIE_SIZE, 40));
            } else if (!hpkt.is_handshake && recv_len > 10) {
                // Parse data packet header for analysis
                size_t eff = ue5::strip_termination(buf, recv_len);
                size_t doff = 38; // OUTER_BITS
                if (eff > doff + 64) {
                    uint32_t pk = static_cast<uint32_t>(ue5::read_bits(buf, recv_len, doff, 32));
                    uint16_t dseq = (pk >> 18) & 0x3FFF;
                    uint16_t dack = (pk >> 4) & 0x3FFF;
                    uint16_t dhist = (pk & 0xF) + 1;
                    for (uint16_t hi = 0; hi < dhist; ++hi)
                        ue5::read_bits(buf, recv_len, doff, 32);
                    // Skip AoC custom field (48 bits = 6 bytes) BEFORE PacketInfo
                    if (doff + 48 <= eff) {
                        uint8_t cf[6];
                        for (int ci = 0; ci < 6; ++ci)
                            cf[ci] = static_cast<uint8_t>(ue5::read_bits(buf, recv_len, doff, 8));
                        spdlog::debug("[GameServer] RELAY   custom field: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                                     cf[0], cf[1], cf[2], cf[3], cf[4], cf[5]);
                    }
                    bool dp = ue5::read_bits(buf, recv_len, doff, 1) != 0;
                    if (dp && doff + 10 <= eff) ue5::read_bits(buf, recv_len, doff, 10);
                    bool ds = ue5::read_bits(buf, recv_len, doff, 1) != 0;
                    uint8_t dft = 0;
                    if (ds && doff + 8 <= eff) dft = static_cast<uint8_t>(ue5::read_bits(buf, recv_len, doff, 8));
                    size_t bb = (eff > doff) ? eff - doff : 0;
                    spdlog::info("[GameServer] RELAY S>C Data {}B Seq={} Ack={} Hist={} PktI={} SrvFT={}/{} Bunch={}b",
                                 recv_len, dseq, dack, dhist, dp, ds, dft, bb);
                    // Dump first bunch header bits
                    if (bb > 0) {
                        std::string bstr;
                        size_t show = std::min(bb, size_t(200));
                        for (size_t bi = 0; bi < show; ++bi) {
                            size_t bidx = (doff + bi) / 8;
                            int bbit = (doff + bi) % 8;
                            if (bidx < static_cast<size_t>(recv_len))
                                bstr += ((buf[bidx] >> bbit) & 1) ? '1' : '0';
                            if ((bi + 1) % 8 == 0) bstr += ' ';
                        }
                        spdlog::info("[GameServer] RELAY   bunch bits: {}", bstr);
                        aoc::protocol::wire::walk_bunches(buf, recv_len, doff, eff,
                            [&](const aoc::protocol::wire::ParsedBunch& hdr) {
                                try_parse_bunch(buf, recv_len, hdr);
                            });
                    }
                } else {
                    spdlog::info("[GameServer] RELAY S>C Data {}B (short)", recv_len);
                }
            }
// [CoA - C.Y.B]

            ProxyLogger::instance().log_udp("S>C", remote, buf, recv_len, "");

            sockaddr_in dest{};
            bool have_client = false;
            {
                std::lock_guard<std::mutex> lk(client_mu_);
                if (client_known_) {
                    dest = last_client_addr_;
                    have_client = true;
                }
            }

            if (have_client) {
                int sent = sendto(sock_, reinterpret_cast<const char*>(buf), recv_len, 0,
                                  reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
                if (sent <= 0)
                    spdlog::warn("[GameServer] Relay to client FAILED");
                else
                    spdlog::info("[GameServer]    -> relayed {}B to client", sent);
            } else {
                spdlog::warn("[GameServer] S>C but no client known — dropping");
            }
        }
        spdlog::info("[GameServer] Upstream loop ended");
    }
};

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
/// ============================================================================
/// TetherService — UDP ARQ Tether Protocol Implementation
/// ============================================================================
///
/// The Ashes of Creation launcher/game tether is a custom ARQ protocol over
/// UDP (NOT gRPC, NOT TCP). When the launcher spawns the game binary it passes:
///
///     AOCClient-Win64-Shipping.exe -LauncherTetherPort=<port>
///
/// The game then:
///   1. Sends a Connect packet to 127.0.0.1:<port>
///   2. Exchanges TetherMessage protobufs over the ARQ channel
///   3. Receives the ICS gRPC endpoint (host:port) from the server
///   4. Connects to that endpoint for all subsequent gRPC services
///
/// ─── Wire Protocol ─────────────────────────────────────────────────────────
///
/// Every UDP packet begins with a 40-byte big-endian header:
///
///   Offset  Size  Field
///     0      4    magicNumber  = (APP_NUMBER<<16) | (version<<8) | messageId
///     4      4    sessionId
///     8      4    windowSize
///    12      2    fragmentNumber
///    14      2    dataLength
///    16      8    sequenceNumber        (int64, signed)
///    24      8    nextExpectedRecvSeq   (int64, signed)
///    32      8    timestampMsec         (int64, epoch ms)
///
/// APP_NUMBER = 42330
/// MessageId  1=Data  2=ACK  3=AskWindow  4=TellWindow  5=Connect
///
/// Connect payload (after 40-byte header, version >= 2):
///   int32BE  processorIdLen
///   bytes    processorId        (game sends "launcher-id-101")
///
/// Data payload (after 40-byte header):
///   TetherMessage protobuf (hand-rolled, see below)
///
/// ─── TetherMessage ─────────────────────────────────────────────────────────
///
/// A minimal hand-rolled protobuf (no gRPC framing, no libprotobuf):
///   field 1  map<string,string> tags      (repeated MapEntry sub-message)
///   field 2  string             data_string
///   field 3  bytes              data_bytes
///
/// The "message_type_name" tag drives dispatch:
///   request_open_session            → reply_open_session
///   request_close_session           → reply_close_session
///   request_echo                    → reply_echo
///   request_game_client_connection_info → reply_game_client_connection_info
///
/// Source: reversed from Patcher.dll + AshesEmulator legacy tether-server
/// research. See memory/project_aoc_protocol.md for full field reference.
/// ============================================================================
// (CoA) (C | Y | B)

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <spdlog/spdlog.h>
#include "services/tether/ics_token_file.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Namespace tether::proto — inline protobuf codec (no libprotobuf dependency)
// ============================================================================

namespace tether::proto {

/// Append a protobuf varint (wire type 0) to `out`.
inline void varint_encode(std::vector<uint8_t>& out, uint64_t v) {
    do {
        uint8_t b = static_cast<uint8_t>(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}

/// Decode a varint from buf[offset..], advancing offset. Returns 0 on error.
inline uint64_t varint_decode(const uint8_t* buf, size_t len, size_t& off) {
    uint64_t r = 0;
    int shift  = 0;
    while (off < len) {
        uint8_t b = buf[off++];
        r |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return r;
        shift += 7;
        if (shift >= 64) break;
    }
    return r;
}

/// Append a length-delimited string field (wire type 2).
inline void field_string(std::vector<uint8_t>& out, uint32_t fn, const std::string& s) {
    varint_encode(out, (static_cast<uint64_t>(fn) << 3) | 2);
    varint_encode(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

/// Append a length-delimited bytes field (wire type 2).
inline void field_bytes(std::vector<uint8_t>& out, uint32_t fn,
                        const std::vector<uint8_t>& b) {
    varint_encode(out, (static_cast<uint64_t>(fn) << 3) | 2);
    varint_encode(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
}

/// Append an embedded message field (wire type 2).
inline void field_message(std::vector<uint8_t>& out, uint32_t fn,
                          const std::vector<uint8_t>& m) {
    varint_encode(out, (static_cast<uint64_t>(fn) << 3) | 2);
    varint_encode(out, m.size());
    out.insert(out.end(), m.begin(), m.end());
}

/// A single decoded proto field.
struct Field {
    uint32_t             field_number = 0;
    int                  wire_type    = 0;  // 0=varint 1=i64 2=len 5=i32
    uint64_t             varint_value = 0;
    std::vector<uint8_t> bytes_value;

    [[nodiscard]] std::string as_string() const {
        return {bytes_value.begin(), bytes_value.end()};
    }
};

/// Decode all proto fields from a buffer. Unknown wire types stop parsing.
inline std::vector<Field> decode(const uint8_t* buf, size_t len) {
    std::vector<Field> out;
    size_t off = 0;
    while (off < len) {
        const size_t saved = off;
        const uint64_t tag = varint_decode(buf, len, off);
        if (off == saved) break;

        Field f;
        f.field_number = static_cast<uint32_t>(tag >> 3);
        f.wire_type    = static_cast<int>(tag & 7);

        if (f.wire_type == 0) {
            f.varint_value = varint_decode(buf, len, off);
        } else if (f.wire_type == 2) {
            const uint64_t sz = varint_decode(buf, len, off);
            if (off + sz <= len) {
                f.bytes_value.assign(buf + off,
                                     buf + off + static_cast<size_t>(sz));
                off += static_cast<size_t>(sz);
            }
        } else if (f.wire_type == 1) {
            if (off + 8 <= len) off += 8;  // fixed64 — skip
        } else if (f.wire_type == 5) {
            if (off + 4 <= len) off += 4;  // fixed32 — skip
        } else {
            break;  // unknown wire type — stop cleanly
        }
        out.push_back(std::move(f));
    }
    return out;
}

} // namespace tether::proto

// ============================================================================
// Namespace tether::wire — ARQ protocol types and header codec
// ============================================================================

namespace tether::wire {

inline constexpr uint32_t APP_NUMBER   = 42330;
inline constexpr size_t   HEADER_SIZE  = 40;
// (Cyb + C.O.A)

// ARQ message identifiers (low byte of magicNumber)
enum class MsgId : uint8_t {
    data        = 1,
    ack         = 2,
    ask_window  = 3,
    tell_window = 4,
    connect     = 5,
};

inline const char* msg_name(MsgId id) noexcept {
    switch (id) {
    case MsgId::data:        return "Data";
    case MsgId::ack:         return "ACK";
    case MsgId::ask_window:  return "AskWindow";
    case MsgId::tell_window: return "TellWindow";
    case MsgId::connect:     return "Connect";
    default:                 return "?";
    }
}

/// Decoded 40-byte packet header.
struct Header {
    uint32_t magic_number           = 0;
    uint32_t session_id             = 0;
    uint32_t window_size            = 0;
    uint16_t fragment_number        = 0;
    uint16_t data_length            = 0;
    int64_t  sequence_number        = 0;
    int64_t  next_expected_recv_seq = 0;
    int64_t  timestamp_msec         = 0;

    [[nodiscard]] uint8_t version()  const noexcept {
        return static_cast<uint8_t>((magic_number >> 8) & 0xFF);
    }
    [[nodiscard]] MsgId   msg_id()   const noexcept {
        return static_cast<MsgId>(magic_number & 0xFF);
    }
    [[nodiscard]] bool    is_valid() const noexcept {
        return (magic_number >> 16) == APP_NUMBER;
    }
};

// Big-endian read/write helpers (protocol is all big-endian)
inline void     w16(uint8_t* p, uint16_t v)  noexcept {
    p[0] = uint8_t(v >> 8); p[1] = uint8_t(v);
}
inline void     w32(uint8_t* p, uint32_t v)  noexcept {
    p[0]=uint8_t(v>>24); p[1]=uint8_t(v>>16); p[2]=uint8_t(v>>8); p[3]=uint8_t(v);
}
inline void     w64(uint8_t* p, int64_t v)   noexcept {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(u >> (56 - 8*i));
}
inline uint16_t r16(const uint8_t* p)        noexcept {
    return uint16_t(uint16_t(p[0]) << 8 | p[1]);
}
inline uint32_t r32(const uint8_t* p)        noexcept {
    return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
inline int64_t  r64(const uint8_t* p)        noexcept {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | p[i];
    return static_cast<int64_t>(u);
}

/// Encode a Header into a 40-byte vector.
inline std::vector<uint8_t> encode(const Header& h) {
    std::vector<uint8_t> b(HEADER_SIZE, 0);
    w32(b.data() +  0, h.magic_number);
    w32(b.data() +  4, h.session_id);
    w32(b.data() +  8, h.window_size);
    w16(b.data() + 12, h.fragment_number);
    w16(b.data() + 14, h.data_length);
    w64(b.data() + 16, h.sequence_number);
    w64(b.data() + 24, h.next_expected_recv_seq);
    w64(b.data() + 32, h.timestamp_msec);
    return b;
}

/// Decode a Header from raw bytes. Returns false if buffer is too short.
inline bool decode(const uint8_t* buf, size_t len, Header& out) {
    if (len < HEADER_SIZE) return false;
    out.magic_number           = r32(buf +  0);
    out.session_id             = r32(buf +  4);
    out.window_size            = r32(buf +  8);
    out.fragment_number        = r16(buf + 12);
    out.data_length            = r16(buf + 14);
    out.sequence_number        = r64(buf + 16);
    out.next_expected_recv_seq = r64(buf + 24);
    out.timestamp_msec         = r64(buf + 32);
    return true;
}

/// Application-level message (fields 1=tags, 2=data_string, 3=data_bytes).
struct Message {
    std::map<std::string, std::string> tags;
    std::string                        data_string;
    std::vector<uint8_t>               data_bytes;

    /// Encode to raw protobuf bytes (no gRPC framing).
    [[nodiscard]] std::vector<uint8_t> encode() const {
        using namespace tether::proto;
        std::vector<uint8_t> out;
        for (const auto& [k, v] : tags) {
            std::vector<uint8_t> entry;
            field_string(entry, 1, k);
            field_string(entry, 2, v);
            field_message(out, 1, entry);
        }
        if (!data_string.empty()) field_string(out, 2, data_string);
        if (!data_bytes.empty())  field_bytes (out, 3, data_bytes);
        return out;
    }

    /// Decode from raw protobuf bytes.
    static Message decode(const uint8_t* buf, size_t len) {
        using namespace tether::proto;
        Message msg;
        for (const auto& f : proto::decode(buf, len)) {
            if (f.field_number == 1 && f.wire_type == 2) {
                std::string k, v;
                for (const auto& ef : proto::decode(
                         f.bytes_value.data(), f.bytes_value.size())) {
                    if      (ef.field_number == 1 && ef.wire_type == 2) k = ef.as_string();
                    else if (ef.field_number == 2 && ef.wire_type == 2) v = ef.as_string();
                }
                if (!k.empty()) msg.tags[k] = v;
            } else if (f.field_number == 2 && f.wire_type == 2) {
                msg.data_string = f.as_string();
            } else if (f.field_number == 3 && f.wire_type == 2) {
                msg.data_bytes = f.bytes_value;
            }
        }
        return msg;
    }
};

} // namespace tether::wire

// ============================================================================
// ARQ Session state
// ============================================================================

struct TetherSession {
    uint32_t    session_id  = 0;
    int64_t     send_seq    = 0;   ///< next outgoing sequence number
    int64_t     recv_seq    = 0;   ///< last sequence number received from peer
    uint32_t    window_size = 64;  ///< peer's advertised receive window
    uint8_t     version     = 2;   ///< protocol version echoed from Connect
    sockaddr_in peer        = {};
    bool        connected            = false;
    bool        connection_info_pushed = false;  ///< true after proactive connection_info push

    /// Ring-buffer of sent data payloads indexed by sequence number.
    /// Allows retransmission when the client sends AskWindow for a seq we've sent.
    static constexpr size_t SENT_WINDOW = 16;
    std::vector<uint8_t> sent_payloads[SENT_WINDOW];  ///< sent_payloads[seq % SENT_WINDOW]
// (Cyb) (C.O.A)

    void store_sent(int64_t seq, const std::vector<uint8_t>& payload) {
        sent_payloads[static_cast<size_t>(seq) % SENT_WINDOW] = payload;
    }
    const std::vector<uint8_t>* get_sent(int64_t seq) const {
        const auto& p = sent_payloads[static_cast<size_t>(seq) % SENT_WINDOW];
        return p.empty() ? nullptr : &p;
    }
};

// ============================================================================
// TetherServiceImpl — main service class
// ============================================================================

class TetherServiceImpl {
public:
    TetherServiceImpl()  = default;
    ~TetherServiceImpl() { stop(); }

    TetherServiceImpl(const TetherServiceImpl&)            = delete;
    TetherServiceImpl& operator=(const TetherServiceImpl&) = delete;

    // ── Configuration ────────────────────────────────────────────────────────

    /// Set the ICS gRPC endpoint that will be advertised to the game client.
    /// Must be called before start().
    void set_ics_endpoint(std::string host, uint16_t port) {
        ics_host_ = std::move(host);
        ics_port_ = port;
    }

    /// Where the launcher saves the player's login token (ics_token_file.h).
    /// Empty (the default) sends the emulator token and never reads the file.
    /// Must be called before start().
    void set_token_file(std::string path) {
        token_file_ = std::move(path);
    }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /// Bind UDP socket and start the receive loop thread. Returns true on success.
    bool start(uint16_t port) {
        // Initialise Winsock (ref-counted, safe to call multiple times)
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);

        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            spdlog::error("[Tether] socket() failed: {}", ::WSAGetLastError());
            return false;
        }

        // Allow quick restart on the same port (matches working AshesEmulator tether)
        const int reuse = 1;
        ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(socket_,
                   reinterpret_cast<const sockaddr*>(&addr),
                   sizeof(addr)) == SOCKET_ERROR) {
            spdlog::error("[Tether] bind() failed on UDP:{}: {}",
                          port, ::WSAGetLastError());
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }

        // Read back the actual port (in case 0 was passed)
        sockaddr_in bound{};
        int blen = sizeof(bound);
        ::getsockname(socket_,
                      reinterpret_cast<sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);

        running_ = true;
        thread_  = std::thread([this] { recv_loop(); });
        spdlog::info("[Tether] UDP ARQ server bound on 0.0.0.0:{}", port_);
        return true;
    }

    /// Signal stop and join the receive thread.
    void stop() {
        if (!running_.exchange(false)) return;
        // Wake the blocking recvfrom with a loopback datagram
        if (socket_ != INVALID_SOCKET) {
            sockaddr_in self{};
            self.sin_family      = AF_INET;
            self.sin_port        = htons(port_);
            self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::sendto(socket_, nullptr, 0, 0,
                     reinterpret_cast<const sockaddr*>(&self), sizeof(self));
        }
        if (thread_.joinable()) thread_.join();
        if (socket_ != INVALID_SOCKET) {
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        spdlog::info("[Tether] stopped");
    }

    [[nodiscard]] bool     is_running() const noexcept { return running_.load(); }
    [[nodiscard]] uint16_t port()       const noexcept { return port_; }

private:
    // ── Timestamp helper ─────────────────────────────────────────────────────

    static int64_t now_msec() noexcept {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
                   system_clock::now().time_since_epoch()).count();
    }

    // ── Formatting helpers ───────────────────────────────────────────────────

    static std::string peer_str(const sockaddr_in& p) {
        char ip[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &p.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(p.sin_port));
    }

    static std::string hex_dump(const uint8_t* data, size_t len,
                                 size_t max_bytes = 64) {
        std::ostringstream oss;
        const size_t n = std::min(len, max_bytes);
        for (size_t i = 0; i < n; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>(data[i]);
            if (i + 1 < n) oss << ' ';
        }
        if (len > max_bytes) oss << "... (" << len << "B)";
        return oss.str();
    }

    // ── Packet builders ───────────────────────────────────────────────────────

    static uint32_t build_magic(uint8_t ver, tether::wire::MsgId id) noexcept {
        return (tether::wire::APP_NUMBER << 16) |
               (static_cast<uint32_t>(ver) << 8) |
                static_cast<uint32_t>(id);
    }

    // Connect response (mirrors the client's Connect back with our processorId)
    std::vector<uint8_t> pkt_connect_response(const TetherSession& s) {
        tether::wire::Header h{};
        h.magic_number           = build_magic(s.version, tether::wire::MsgId::connect);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.sequence_number        = 0;
        h.next_expected_recv_seq = s.recv_seq;
        h.timestamp_msec         = now_msec();
        auto pkt = tether::wire::encode(h);
// [Cyb x C-O-A]

        if (s.version >= 2) {
            static constexpr char     PID[]    = "launcher-id-101";
            static constexpr uint32_t PID_LEN  = sizeof(PID) - 1;
            uint8_t lb[4];
            tether::wire::w32(lb, PID_LEN);
            pkt.insert(pkt.end(), lb, lb + 4);
            pkt.insert(pkt.end(),
                       reinterpret_cast<const uint8_t*>(PID),
                       reinterpret_cast<const uint8_t*>(PID) + PID_LEN);
        }
        return pkt;
    }

    // ACK packet
    std::vector<uint8_t> pkt_ack(const TetherSession& s) {
        tether::wire::Header h{};
        h.magic_number           = build_magic(s.version, tether::wire::MsgId::ack);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.sequence_number        = s.recv_seq;
        h.next_expected_recv_seq = s.recv_seq + 1;
        h.timestamp_msec         = now_msec();
        return tether::wire::encode(h);
    }

    // TellWindow packet
    std::vector<uint8_t> pkt_tell_window(const TetherSession& s) {
        tether::wire::Header h{};
        h.magic_number           = build_magic(s.version, tether::wire::MsgId::tell_window);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.next_expected_recv_seq = s.recv_seq + 1;
        h.timestamp_msec         = now_msec();
        return tether::wire::encode(h);
    }

    // Data packet wrapping a TetherMessage payload
    std::vector<uint8_t> pkt_data(TetherSession& s,
                                   const std::vector<uint8_t>& payload) {
        tether::wire::Header h{};
        h.magic_number           = build_magic(s.version, tether::wire::MsgId::data);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.data_length            = static_cast<uint16_t>(payload.size());
        h.sequence_number        = s.send_seq++;
        h.next_expected_recv_seq = s.recv_seq + 1;
        h.timestamp_msec         = now_msec();
        // Store payload in ring buffer so we can retransmit on AskWindow
        s.store_sent(h.sequence_number, payload);
        auto pkt = tether::wire::encode(h);
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        return pkt;
    }

    // ── Raw socket send ───────────────────────────────────────────────────────

    void send_raw(const std::vector<uint8_t>& pkt, const sockaddr_in& peer) {
        const int n = ::sendto(
            socket_,
            reinterpret_cast<const char*>(pkt.data()),
            static_cast<int>(pkt.size()),
            0,
            reinterpret_cast<const sockaddr*>(&peer),
            sizeof(peer));
        if (n == SOCKET_ERROR)
            spdlog::error("[Tether] sendto error: {}", ::WSAGetLastError());
    }

    /// Encode a TetherMessage reply and send it as a Data packet.
    void send_reply(TetherSession& s, const tether::wire::Message& msg) {
        send_raw(pkt_data(s, msg.encode()), s.peer);
    }

    // ── Receive loop ──────────────────────────────────────────────────────────

    void recv_loop() {
        std::vector<uint8_t> buf(65535);
        while (running_) {
            sockaddr_in peer{};
            int plen = sizeof(peer);
            const int n = ::recvfrom(
                socket_,
                reinterpret_cast<char*>(buf.data()),
                static_cast<int>(buf.size()),
                0,
                reinterpret_cast<sockaddr*>(&peer),
                &plen);

            if (!running_) break;
            if (n == SOCKET_ERROR) {
                const int err = ::WSAGetLastError();
                if (err == WSAEINTR || err == WSAEWOULDBLOCK) continue;
                spdlog::error("[Tether] recvfrom error: {}", err);
                continue;
            }
            if (n > 0) dispatch(buf.data(), static_cast<size_t>(n), peer);
        }
    }

    // ── Top-level packet dispatcher ───────────────────────────────────────────

    void dispatch(const uint8_t* buf, size_t len, const sockaddr_in& peer) {
        tether::wire::Header hdr{};
        if (!tether::wire::decode(buf, len, hdr) || !hdr.is_valid()) return;

        spdlog::info("[Tether] RX {} {} session={} seq={} nextRecv={} window={}",
                     peer_str(peer),
                     tether::wire::msg_name(hdr.msg_id()),
                     hdr.session_id,
                     hdr.sequence_number,
                     hdr.next_expected_recv_seq,
                     hdr.window_size);

        const uint8_t* payload     = buf + tether::wire::HEADER_SIZE;
        const size_t   payload_len =
            len > tether::wire::HEADER_SIZE ? len - tether::wire::HEADER_SIZE : 0;

        switch (hdr.msg_id()) {
        case tether::wire::MsgId::connect:
            on_connect(hdr, payload, payload_len, peer);
            break;

        case tether::wire::MsgId::data:
            if (session_.connected && session_.session_id == hdr.session_id)
                on_data(hdr, payload, payload_len);
            else
                spdlog::warn("[Tether] Data for unknown session {}", hdr.session_id);
            break;

        case tether::wire::MsgId::ack:
            // ACKs advance the send window; no retransmit needed in emulator
            break;
// [Cyb :: C.O.A]

        case tether::wire::MsgId::ask_window:
            if (session_.connected && session_.session_id == hdr.session_id) {
                // hdr.next_expected_recv_seq is the seq the client wants from us.
                // If we've already sent it, retransmit it; otherwise TellWindow.
                const int64_t wanted = hdr.next_expected_recv_seq;
                if (wanted < session_.send_seq) {
                    // Client wants a seq we already sent — retransmit it.
                    const auto* payload = session_.get_sent(wanted);
                    if (payload) {
                        spdlog::debug("[Tether] AskWindow: retransmitting seq={}", wanted);
                        // Re-build the data packet with the stored payload
                        tether::wire::Header rh{};
                        rh.magic_number           = build_magic(session_.version,
                                                                 tether::wire::MsgId::data);
                        rh.session_id             = session_.session_id;
                        rh.window_size            = 64;
                        rh.data_length            = static_cast<uint16_t>(payload->size());
                        rh.sequence_number        = wanted;
                        rh.next_expected_recv_seq = session_.recv_seq + 1;
                        rh.timestamp_msec         = now_msec();
                        auto pkt = tether::wire::encode(rh);
                        pkt.insert(pkt.end(), payload->begin(), payload->end());
                        send_raw(pkt, session_.peer);
                    } else {
                        send_raw(pkt_tell_window(session_), session_.peer);
                    }
                } else {
                    // Client wants data we have not sent. Keep this at the
                    // ARQ layer; unsolicited application replies are logged by
                    // the client as unexpected tether messages.
                    send_raw(pkt_tell_window(session_), session_.peer);
                }
            }
            break;

        case tether::wire::MsgId::tell_window:
            if (session_.connected && session_.session_id == hdr.session_id)
                session_.window_size = hdr.window_size;
            break;

        default:
            spdlog::warn("[Tether] Unknown MsgId 0x{:02x}  hex={}",
                         static_cast<int>(hdr.msg_id()),
                         hex_dump(buf, std::min(len, size_t(16))));
            break;
        }
    }

    // ── ARQ handlers ─────────────────────────────────────────────────────────

    void on_connect(const tether::wire::Header& hdr,
                    const uint8_t* payload, size_t plen,
                    const sockaddr_in& peer) {
        // Parse Connect payload: [int32BE addrLen + addrBytes] [int32BE pidLen + pidBytes]
        size_t off = 0;
        std::string remote_addr, processor_id;

        if (off + 4 <= plen) {
            const uint32_t alen = tether::wire::r32(payload + off); off += 4;
            if (off + alen <= plen) {
                remote_addr.assign(reinterpret_cast<const char*>(payload + off), alen);
                off += alen;
            }
        }
        if (hdr.version() >= 2 && off + 4 <= plen) {
            const uint32_t pil = tether::wire::r32(payload + off); off += 4;
            if (off + pil <= plen) {
                processor_id.assign(reinterpret_cast<const char*>(payload + off), pil);
            }
        }

        spdlog::info("[Tether] Connect: remote='{}' processorId='{}' ver={}",
                     remote_addr, processor_id, hdr.version());

        // Accept the session (single-session: replace any previous)
        session_.session_id              = hdr.session_id;
        session_.recv_seq                = hdr.sequence_number;
        session_.send_seq                = 0;
        session_.window_size             = hdr.window_size > 0 ? hdr.window_size : 64;
        session_.version                 = hdr.version();
        session_.connection_info_pushed  = false;
        session_.peer        = peer;
        session_.connected   = true;

        send_raw(pkt_connect_response(session_), peer);
        spdlog::info("[Tether] Session {} established (ver={} window={})",
                     session_.session_id, session_.version, session_.window_size);
    }

    void on_data(const tether::wire::Header& hdr,
                 const uint8_t* payload, size_t plen) {
        session_.recv_seq = hdr.sequence_number;
        send_raw(pkt_ack(session_), session_.peer);  // ACK before processing

        if (plen == 0) return;

        auto msg = tether::wire::Message::decode(payload, plen);

        // Debug: log all tags
        spdlog::debug("[Tether] TetherMessage tags={} data_string={}B data_bytes={}B",
                      msg.tags.size(), msg.data_string.size(), msg.data_bytes.size());
        for (const auto& [k, v] : msg.tags)
            spdlog::debug("[Tether]   [{}] = '{}'", k, v);

        // Dispatch on message_type_name tag
        auto it = msg.tags.find("message_type_name");
        if (it == msg.tags.end()) {
            spdlog::warn("[Tether] TetherMessage missing 'message_type_name' — ignoring");
            return;
        }

        const std::string& type = it->second;
        spdlog::info("[Tether] Dispatch '{}'", type);
// C | O | A / Cyb

        if      (type == "request_open_session")
            handle_open_session(msg);
        else if (type == "request_close_session")
            handle_close_session(msg);
        else if (type == "request_echo")
            handle_echo(msg);
        else if (type == "request_game_client_connection_info")
            handle_connection_info(msg);
        else
            spdlog::warn("[Tether] Unknown message type '{}'", type);
    }

    // ── Application-level message handlers ───────────────────────────────────

    /// Game/launcher requests a session — reply with auth tokens and endpoints.
    ///
    /// Sends one echoed request_open_session with auth tokens and local ICS
    /// endpoint tags. Extra tether replies are treated as unexpected by the
    /// current client build.
    void handle_open_session(const tether::wire::Message& req) {
        spdlog::info("[Tether] open_session: sending echoed request_open_session with local ICS endpoint");

        // ── Shared auth tags ─────────────────────────────────────────────────
        // Auth tokens (emulated — real values come from a real auth server)
        const std::string port_s          = std::to_string(ics_port_);
        const std::string ics_endpoint    = ics_host_ + ":" + port_s;
        const std::string auth_endpoint   = "https://" + ics_endpoint;
        // With --login-token-file (the Go stack) the launcher saves the
        // player's login token (ics_token_file.h) and the Go gateway resolves
        // it to the account; only a client on this machine gets it. Otherwise
        // the client gets the emulator token, which aoc_server's own ICS
        // accepts and the gateway refuses.
        const bool peer_is_loopback =
            aoc::ics_token::is_loopback_ipv4(ntohl(session_.peer.sin_addr.s_addr));
        const auto tokens = aoc::ics_token::client_tokens(token_file_, peer_is_loopback);
        if (!token_file_.empty()) log_token_choice(tokens.saved, peer_is_loopback);
        const std::string& auth_token    = tokens.auth;
        const std::string& refresh_token = tokens.refresh;
        const std::string auth0_token     = "emulator-auth0-deadbeef";

        // ── seq=0: echo the client's request back (receipt confirmation) ─────
        tether::wire::Message echo;
        echo.tags              = req.tags;        // echo correlation tags (incl. message_type_name)
        echo.data_string       = req.data_string;
        echo.data_bytes        = req.data_bytes;
        echo.tags["auth_endpoint"]           = auth_endpoint;
        echo.tags["auth_token"]              = auth_token;
        echo.tags["refresh_token"]           = refresh_token;
        echo.tags["auth0_accesstoken"]       = auth0_token;
        echo.tags["user_agent_prefix"]       = "AOC/1.0";
        echo.tags["grpc_metadata_magic_key"] = "x-ics-auth";
        echo.tags["grpc_metadata_magic_val"] = "emulator-magic";
        echo.tags["eac_deployment_id"]       = "aoc-emulator";
        echo.tags["eac_sandbox_id"]          = "emulator-sandbox";
        echo.tags["ics_host"]                = ics_host_;
        echo.tags["ics_port"]                = port_s;
        echo.tags["ics_endpoint"]            = ics_endpoint;
        echo.tags["ics_address"]             = ics_endpoint;
        echo.tags["connection_address"]      = ics_endpoint;
        echo.tags["connection_host"]         = ics_host_;
        echo.tags["connection_port"]         = port_s;
        send_reply(session_, echo);  // server seq=0

    }

    /// Says which token open_session handed out (never the token itself).
    /// Only called when saved login tokens are enabled.
    void log_token_choice(bool saved, bool peer_is_loopback) const {
        if (!peer_is_loopback) {
            spdlog::warn("[Tether] open_session: the client at {} is not on this machine; "
                         "sending the emulator token, not the saved login token",
                         peer_str(session_.peer));
            return;
        }
        if (!saved) {
            spdlog::info("[Tether] open_session: no saved login token in {}, sending the emulator token "
                         "(the gateway refuses it: sign in through the launcher)", token_file_);
            return;
        }
        const auto age = aoc::ics_token::age(token_file_);
        spdlog::info("[Tether] open_session: sending the launcher's saved login token (saved {} ago)",
                     age ? aoc::ics_token::format_age(*age) : std::string("an unknown time"));
        if (age && *age > aoc::ics_token::kDefaultTokenLifetime)
            spdlog::warn("[Tether] open_session: the saved login token is older than 24 h, auth's default "
                         "token lifetime; if the gateway refuses it, sign in again through the launcher");
    }

    /// Game/launcher is closing — acknowledge and disconnect.
    void handle_close_session(const tether::wire::Message& /*req*/) {
        spdlog::info("[Tether] close_session: disconnecting");

        tether::wire::Message reply;
        reply.tags["message_type_name"] = "reply_close_session";
        reply.tags["type"]              = "reply_close_session";
        send_reply(session_, reply);

        session_.connected = false;
    }

    /// Echo request — return all tags unchanged.
    void handle_echo(const tether::wire::Message& req) {
        spdlog::debug("[Tether] echo request");

        tether::wire::Message reply;
        reply.tags              = req.tags;
        reply.data_string       = req.data_string;
        reply.data_bytes        = req.data_bytes;
        reply.tags["message_type_name"] = "reply_echo";
        reply.tags["type"]              = "reply_echo";

        send_reply(session_, reply);
    }

    /// Game requests ICS gRPC connection info — return host:port from config.
    void handle_connection_info(const tether::wire::Message& req) {
        const std::string port_s = std::to_string(ics_port_);
        spdlog::info("[Tether] connection_info: advertising ICS {}:{}", ics_host_, ics_port_);

        tether::wire::Message reply;
        reply.tags              = req.tags;        // echo correlation tags
        reply.data_string       = req.data_string;
        reply.data_bytes        = req.data_bytes;

        reply.tags["message_type_name"]  = "reply_game_client_connection_info";
        reply.tags["type"]               = "reply_game_client_connection_info";
        reply.tags["result"]             = "ok";

        // Provide endpoint under every known key name so the client finds it
        // regardless of which field it reads (different game builds may vary).
        reply.tags["ics_host"]           = ics_host_;
        reply.tags["ics_port"]           = port_s;
        reply.tags["ics_endpoint"]       = ics_host_ + ":" + port_s;
        reply.tags["ics_address"]        = ics_host_ + ":" + port_s;
        reply.tags["connection_address"] = ics_host_ + ":" + port_s;
        reply.tags["connection_host"]    = ics_host_;
        reply.tags["connection_port"]    = port_s;

        send_reply(session_, reply);
    }
// -- CoA . C-Y-B --

    // ── Members ───────────────────────────────────────────────────────────────

    SOCKET             socket_  = INVALID_SOCKET;
    uint16_t           port_    = 0;
    std::atomic<bool>  running_ = false;
    std::thread        thread_;

    TetherSession      session_;       ///< Current ARQ session (single-client)

    // ICS gRPC endpoint advertised to the game via connection_info reply
    std::string        ics_host_ = "127.0.0.1";
    uint16_t           ics_port_ = 443;
    std::string        token_file_;
};

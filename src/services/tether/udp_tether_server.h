// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
/// ============================================================================
/// UDP ARQ Tether Server
/// ============================================================================
///
/// The game client connects here after being launched with
///   -LauncherTetherPort=<port>
///
/// Protocol: custom ARQ over UDP. Every packet has a 40-byte big-endian
/// header followed by an optional TetherMessage protobuf payload.
///
/// Header layout (40 bytes, big-endian):
///   uint32  magicNumber = (42330 << 16) | (version << 8) | messageId
///   uint32  sessionId
///   uint32  windowSize
///   uint16  fragmentNumber
///   uint16  dataLength
///   int64   sequenceNumber
///   int64   nextExpectedRecvSequenceNumber
///   int64   timestampMsec
///
/// MessageIds: 1=Data, 2=ACK, 3=AskWindow, 4=TellWindow, 5=Connect
///
/// Source: reversed from Patcher.dll + legacy tether-server research.
/// ============================================================================

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
// (C-O-A ~ C_Y_B)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

// ─── Inline proto codec (no gRPC, no Protobuf library) ──────────────────────

namespace tether_proto {

inline void varint_encode(std::vector<uint8_t>& out, uint64_t value) {
    do {
        uint8_t b = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value) b |= 0x80;
        out.push_back(b);
    } while (value);
}

inline uint64_t varint_decode(const uint8_t* buf, size_t len, size_t& offset) {
    uint64_t result = 0;
    int      shift  = 0;
    while (offset < len) {
        uint8_t b = buf[offset++];
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return result;
        shift += 7;
        if (shift >= 64) break;
    }
    return result;
}

// wire type 2 (length-delimited) helpers
inline void field_string(std::vector<uint8_t>& out, uint32_t fn, const std::string& s) {
    varint_encode(out, (static_cast<uint64_t>(fn) << 3) | 2);
    varint_encode(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

inline void field_bytes(std::vector<uint8_t>& out, uint32_t fn, const std::vector<uint8_t>& b) {
    varint_encode(out, (static_cast<uint64_t>(fn) << 3) | 2);
    varint_encode(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
}

inline void field_message(std::vector<uint8_t>& out, uint32_t fn, const std::vector<uint8_t>& m) {
    varint_encode(out, (static_cast<uint64_t>(fn) << 3) | 2);
    varint_encode(out, m.size());
    out.insert(out.end(), m.begin(), m.end());
}

struct ProtoField {
    uint32_t             field_number = 0;
    int                  wire_type    = 0;
    uint64_t             varint_value = 0;
    std::vector<uint8_t> bytes_value;
    std::string as_string() const { return {bytes_value.begin(), bytes_value.end()}; }
};

inline std::vector<ProtoField> decode_proto(const uint8_t* buf, size_t len) {
    std::vector<ProtoField> fields;
    size_t offset = 0;
    while (offset < len) {
        size_t   saved = offset;
        uint64_t tag   = varint_decode(buf, len, offset);
        if (offset == saved) break;

        ProtoField f;
        f.field_number = static_cast<uint32_t>(tag >> 3);
        f.wire_type    = static_cast<int>(tag & 7);
// Cyb - C-O-A

        if (f.wire_type == 0) {
            f.varint_value = varint_decode(buf, len, offset);
        } else if (f.wire_type == 2) {
            uint64_t sz = varint_decode(buf, len, offset);
            if (offset + sz <= len) {
                f.bytes_value.assign(buf + offset, buf + offset + static_cast<size_t>(sz));
                offset += static_cast<size_t>(sz);
            }
        } else if (f.wire_type == 1) {
            if (offset + 8 <= len) offset += 8;
        } else if (f.wire_type == 5) {
            if (offset + 4 <= len) offset += 4;
        } else {
            break;
        }
        fields.push_back(std::move(f));
    }
    return fields;
}

} // namespace tether_proto

// ─── Protocol constants ──────────────────────────────────────────────────────

static constexpr uint32_t TETHER_APP_NUMBER  = 42330;
static constexpr size_t   TETHER_HEADER_SIZE = 40;

enum class TetherMsgId : uint8_t {
    data        = 1,
    ack         = 2,
    ask_window  = 3,
    tell_window = 4,
    connect     = 5,
};

struct TetherHdr {
    uint32_t magic_number           = 0;
    uint32_t session_id             = 0;
    uint32_t window_size            = 0;
    uint16_t fragment_number        = 0;
    uint16_t data_length            = 0;
    int64_t  sequence_number        = 0;
    int64_t  next_expected_recv_seq = 0;
    int64_t  timestamp_msec         = 0;

    uint8_t     version()    const noexcept { return static_cast<uint8_t>((magic_number >> 8) & 0xFF); }
    TetherMsgId message_id() const noexcept { return static_cast<TetherMsgId>(magic_number & 0xFF); }
    bool        is_valid()   const noexcept { return (magic_number >> 16) == TETHER_APP_NUMBER; }
};

struct TetherMsg {
    std::map<std::string, std::string> tags;
    std::string                        data_string;
    std::vector<uint8_t>               data_bytes;
};

struct TetherSession {
    uint32_t    session_id  = 0;
    int64_t     send_seq    = 0;
    int64_t     recv_seq    = 0;
    uint32_t    window_size = 64;
    uint8_t     version     = 2;
    sockaddr_in peer        = {};
    bool        connected   = false;
};

// ─── UdpTetherServer ─────────────────────────────────────────────────────────

class UdpTetherServer {
public:
    UdpTetherServer() = default;
    ~UdpTetherServer() { stop(); }

    UdpTetherServer(const UdpTetherServer&)            = delete;
    UdpTetherServer& operator=(const UdpTetherServer&) = delete;

    void set_ics_endpoint(std::string host, uint16_t port) {
        ics_host_ = std::move(host);
        ics_port_ = port;
    }

    bool start(uint16_t port) {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);  // ref-counted, safe to call multiple times

        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            spdlog::error("[Tether] socket() failed: {}", ::WSAGetLastError());
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(socket_,
                   reinterpret_cast<const sockaddr*>(&addr),
                   sizeof(addr)) == SOCKET_ERROR) {
            spdlog::error("[Tether] bind() failed on UDP:{}: {}", port, ::WSAGetLastError());
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }

        // Read back actual bound port
        sockaddr_in bound{};
        int blen = sizeof(bound);
        ::getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);

        running_ = true;
        thread_  = std::thread([this] { run_loop(); });
        spdlog::info("[Tether] UDP ARQ server bound on 0.0.0.0:{}", port_);
        return true;
    }

    bool is_running() const noexcept { return running_.load(); }

    void stop() {
        if (!running_.exchange(false)) return;
        if (socket_ != INVALID_SOCKET) {
            // Wake up blocking recvfrom
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
        spdlog::info("[Tether] UDP ARQ server stopped");
    }
// [C.Y.B x C.O.A]

private:
    // ── Helpers ──────────────────────────────────────────────────────────────

    static int64_t now_msec() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
                   system_clock::now().time_since_epoch()).count();
    }

    static std::string peer_str(const sockaddr_in& p) {
        char ip[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &p.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(p.sin_port));
    }

    static const char* msgid_name(TetherMsgId id) {
        switch (id) {
        case TetherMsgId::data:        return "Data";
        case TetherMsgId::ack:         return "ACK";
        case TetherMsgId::ask_window:  return "AskWindow";
        case TetherMsgId::tell_window: return "TellWindow";
        case TetherMsgId::connect:     return "Connect";
        default:                       return "Unknown";
        }
    }

    // ── Wire helpers ─────────────────────────────────────────────────────────

    static void w16(uint8_t* p, uint16_t v) {
        p[0] = static_cast<uint8_t>(v >> 8);
        p[1] = static_cast<uint8_t>(v);
    }
    static void w32(uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >>  8);
        p[3] = static_cast<uint8_t>(v);
    }
    static void w64(uint8_t* p, int64_t v) {
        const uint64_t u = static_cast<uint64_t>(v);
        p[0] = static_cast<uint8_t>(u >> 56); p[1] = static_cast<uint8_t>(u >> 48);
        p[2] = static_cast<uint8_t>(u >> 40); p[3] = static_cast<uint8_t>(u >> 32);
        p[4] = static_cast<uint8_t>(u >> 24); p[5] = static_cast<uint8_t>(u >> 16);
        p[6] = static_cast<uint8_t>(u >>  8); p[7] = static_cast<uint8_t>(u);
    }
    static uint16_t r16(const uint8_t* p) {
        return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
    }
    static uint32_t r32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
    }
    static int64_t r64(const uint8_t* p) {
        uint64_t u = (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) |
                     (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
                     (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) |
                     (uint64_t(p[6]) <<  8) |  uint64_t(p[7]);
        return static_cast<int64_t>(u);
    }

    // ── Header encode / decode ────────────────────────────────────────────────

    static std::vector<uint8_t> encode_hdr(const TetherHdr& h) {
        std::vector<uint8_t> b(TETHER_HEADER_SIZE, 0);
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
// C.O.A : Cyb

    static bool decode_hdr(const uint8_t* buf, size_t len, TetherHdr& out) {
        if (len < TETHER_HEADER_SIZE) return false;
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

    // ── TetherMessage encode / decode ─────────────────────────────────────────

    static std::vector<uint8_t> encode_msg(const TetherMsg& m) {
        using namespace tether_proto;
        std::vector<uint8_t> out;
        for (const auto& [k, v] : m.tags) {
            std::vector<uint8_t> entry;
            field_string(entry, 1, k);
            field_string(entry, 2, v);
            field_message(out, 1, entry);
        }
        if (!m.data_string.empty()) field_string(out, 2, m.data_string);
        if (!m.data_bytes.empty())  field_bytes (out, 3, m.data_bytes);
        return out;
    }

    static TetherMsg decode_msg(const uint8_t* buf, size_t len) {
        using namespace tether_proto;
        TetherMsg msg;
        for (const auto& f : decode_proto(buf, len)) {
            if (f.field_number == 1 && f.wire_type == 2) {
                std::string key, val;
                for (const auto& ef : decode_proto(f.bytes_value.data(), f.bytes_value.size())) {
                    if (ef.field_number == 1 && ef.wire_type == 2)
                        key = ef.as_string();
                    else if (ef.field_number == 2 && ef.wire_type == 2)
                        val = ef.as_string();
                }
                if (!key.empty()) msg.tags[key] = val;
            } else if (f.field_number == 2 && f.wire_type == 2) {
                msg.data_string = f.as_string();
            } else if (f.field_number == 3 && f.wire_type == 2) {
                msg.data_bytes = f.bytes_value;
            }
        }
        return msg;
    }

    // ── Packet builders ───────────────────────────────────────────────────────

    static uint32_t build_magic(uint8_t ver, TetherMsgId id) {
        return (TETHER_APP_NUMBER << 16) |
               (static_cast<uint32_t>(ver) << 8) |
                static_cast<uint32_t>(id);
    }

    std::vector<uint8_t> build_connect_response(const TetherSession& s) {
        TetherHdr h{};
        h.magic_number           = build_magic(s.version, TetherMsgId::connect);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.fragment_number        = 0;
        h.data_length            = 0;
        h.sequence_number        = 0;
        h.next_expected_recv_seq = s.recv_seq;
        h.timestamp_msec         = now_msec();
        auto pkt = encode_hdr(h);
        if (s.version >= 2) {
            // Append: int32BE len + processorId bytes
            static constexpr char PID[] = "launcher-id-101";
            static constexpr uint32_t PID_LEN = sizeof(PID) - 1;
            uint8_t lb[4];
            w32(lb, PID_LEN);
            pkt.insert(pkt.end(), lb, lb + 4);
            pkt.insert(pkt.end(),
                       reinterpret_cast<const uint8_t*>(PID),
                       reinterpret_cast<const uint8_t*>(PID) + PID_LEN);
        }
        return pkt;
    }

    std::vector<uint8_t> build_ack(const TetherSession& s) {
        TetherHdr h{};
        h.magic_number           = build_magic(s.version, TetherMsgId::ack);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.sequence_number        = s.recv_seq;
        h.next_expected_recv_seq = s.recv_seq + 1;
        h.timestamp_msec         = now_msec();
        return encode_hdr(h);
    }

    std::vector<uint8_t> build_tell_window(const TetherSession& s) {
        TetherHdr h{};
        h.magic_number           = build_magic(s.version, TetherMsgId::tell_window);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.next_expected_recv_seq = s.recv_seq + 1;
        h.timestamp_msec         = now_msec();
        return encode_hdr(h);
    }

    std::vector<uint8_t> build_data(TetherSession& s, const std::vector<uint8_t>& payload) {
        TetherHdr h{};
        h.magic_number           = build_magic(s.version, TetherMsgId::data);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.data_length            = static_cast<uint16_t>(payload.size());
        h.sequence_number        = s.send_seq++;
        h.next_expected_recv_seq = s.recv_seq + 1;
        h.timestamp_msec         = now_msec();
        auto pkt = encode_hdr(h);
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        return pkt;
    }

    // ── Raw send ─────────────────────────────────────────────────────────────

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

    void send_reply(TetherSession& s, const TetherMsg& reply) {
        send_raw(build_data(s, encode_msg(reply)), s.peer);
    }

    // ── Receive loop ─────────────────────────────────────────────────────────

    void run_loop() {
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
// ~ C-Y-B & CoA ~

            if (!running_) break;
            if (n == SOCKET_ERROR) {
                int err = ::WSAGetLastError();
                if (err == WSAEINTR || err == WSAEWOULDBLOCK) continue;
                spdlog::error("[Tether] recvfrom error: {}", err);
                continue;
            }
            if (n > 0) handle_packet(buf.data(), static_cast<size_t>(n), peer);
        }
    }

    // ── Packet dispatcher ────────────────────────────────────────────────────

    void handle_packet(const uint8_t* buf, size_t len, const sockaddr_in& peer) {
        TetherHdr hdr{};
        if (!decode_hdr(buf, len, hdr) || !hdr.is_valid()) return;

        spdlog::info("[Tether] RX {} {} session={} seq={} nextRecv={}",
                     peer_str(peer), msgid_name(hdr.message_id()),
                     hdr.session_id, hdr.sequence_number,
                     hdr.next_expected_recv_seq);

        const uint8_t* payload     = buf + TETHER_HEADER_SIZE;
        const size_t   payload_len = len > TETHER_HEADER_SIZE ? len - TETHER_HEADER_SIZE : 0;

        switch (hdr.message_id()) {
        case TetherMsgId::connect:
            on_connect(hdr, payload, payload_len, peer);
            break;
        case TetherMsgId::data:
            if (session_.connected && session_.session_id == hdr.session_id)
                on_data(hdr, payload, payload_len);
            break;
        case TetherMsgId::ack:
            // ACKs just advance the send window — no retransmit needed in emulator
            break;
        case TetherMsgId::ask_window:
            if (session_.connected && session_.session_id == hdr.session_id)
                send_raw(build_tell_window(session_), session_.peer);
            break;
        case TetherMsgId::tell_window:
            if (session_.connected && session_.session_id == hdr.session_id)
                session_.window_size = hdr.window_size;
            break;
        default:
            spdlog::warn("[Tether] Unknown MessageId 0x{:02x}", static_cast<int>(hdr.message_id()));
            break;
        }
    }

    // ── Connect ──────────────────────────────────────────────────────────────

    void on_connect(const TetherHdr& hdr, const uint8_t* payload, size_t plen,
                    const sockaddr_in& peer) {
        size_t off = 0;
        std::string remote_addr, processor_id;

        if (off + 4 <= plen) {
            uint32_t alen = r32(payload + off); off += 4;
            if (off + alen <= plen) {
                remote_addr.assign(reinterpret_cast<const char*>(payload + off), alen);
                off += alen;
            }
        }
        if (hdr.version() >= 2 && off + 4 <= plen) {
            uint32_t plen2 = r32(payload + off); off += 4;
            if (off + plen2 <= plen) {
                processor_id.assign(reinterpret_cast<const char*>(payload + off), plen2);
            }
        }

        spdlog::info("[Tether] Connect: remote='{}' processorId='{}' ver={}",
                     remote_addr, processor_id, hdr.version());

        session_.session_id  = hdr.session_id;
        session_.recv_seq    = hdr.sequence_number;
        session_.send_seq    = 0;
        session_.window_size = hdr.window_size > 0 ? hdr.window_size : 64;
        session_.version     = hdr.version();
        session_.peer        = peer;
        session_.connected   = true;

        send_raw(build_connect_response(session_), peer);
        spdlog::info("[Tether] Session {} established (ver={} window={})",
                     session_.session_id, session_.version, session_.window_size);
    }

    // ── Data ─────────────────────────────────────────────────────────────────

    void on_data(const TetherHdr& hdr, const uint8_t* payload, size_t plen) {
        session_.recv_seq = hdr.sequence_number;
        send_raw(build_ack(session_), session_.peer);  // ACK first

        if (plen == 0) return;

        TetherMsg msg = decode_msg(payload, plen);

        spdlog::debug("[Tether] TetherMessage: tags={} data_string={}B data_bytes={}B",
                      msg.tags.size(), msg.data_string.size(), msg.data_bytes.size());
        for (const auto& [k, v] : msg.tags)
            spdlog::debug("[Tether]   tag['{}'] = '{}'", k, v);

        auto it = msg.tags.find("message_type_name");
        if (it == msg.tags.end()) {
            spdlog::warn("[Tether] TetherMessage missing 'message_type_name'");
            return;
        }
        const std::string& type = it->second;
        spdlog::info("[Tether] Dispatch type='{}'", type);

        if (type == "request_open_session")
            on_open_session(msg);
        else if (type == "request_close_session")
            on_close_session(msg);
        else if (type == "request_echo")
            on_echo(msg);
        else if (type == "request_game_client_connection_info")
            on_game_client_connection_info(msg);
        else
            spdlog::warn("[Tether] Unknown tether message type '{}'", type);
    }

    // ── Application handlers ─────────────────────────────────────────────────

    void on_open_session(const TetherMsg& req) {
        spdlog::info("[Tether] open_session → sending auth tokens");

        TetherMsg reply;
        reply.tags              = req.tags;   // echo back correlation tags
        reply.data_string       = req.data_string;
        reply.data_bytes        = req.data_bytes;
// (C Y B) (CoA)

        reply.tags["message_type_name"]       = "reply_open_session";
        reply.tags["auth_endpoint"]           = "https://release-global.ashesofcreation.com";
        reply.tags["auth_token"]              = "emulator-ics-token-deadbeef";
        reply.tags["refresh_token"]           = "emulator-refresh-deadbeef";
        reply.tags["auth0_accesstoken"]       = "emulator-auth0-deadbeef";
        reply.tags["user_agent_prefix"]       = "AOC/1.0";
        reply.tags["grpc_metadata_magic_key"] = "x-ics-auth";
        reply.tags["grpc_metadata_magic_val"] = "emulator-magic";
        reply.tags["eac_deployment_id"]       = "aoc-emulator";
        reply.tags["eac_sandbox_id"]          = "emulator-sandbox";

        send_reply(session_, reply);
    }

    void on_close_session(const TetherMsg& /*req*/) {
        spdlog::info("[Tether] close_session");

        TetherMsg reply;
        reply.tags["message_type_name"] = "reply_close_session";
        reply.tags["type"]              = "reply_close_session";
        send_reply(session_, reply);

        session_.connected = false;
    }

    void on_echo(const TetherMsg& req) {
        spdlog::debug("[Tether] echo");

        TetherMsg reply;
        reply.tags              = req.tags;
        reply.data_string       = req.data_string;
        reply.data_bytes        = req.data_bytes;
        reply.tags["type"]              = "reply_echo";
        reply.tags["message_type_name"] = "reply_echo";
        send_reply(session_, reply);
    }

    void on_game_client_connection_info(const TetherMsg& req) {
        const std::string port_s = std::to_string(ics_port_);
        spdlog::info("[Tether] game_client_connection_info → {}:{}", ics_host_, port_s);

        TetherMsg reply;
        reply.tags              = req.tags;   // echo correlation tags
        reply.data_string       = req.data_string;
        reply.data_bytes        = req.data_bytes;

        reply.tags["message_type_name"]  = "reply_game_client_connection_info";
        reply.tags["type"]               = "reply_game_client_connection_info";
        reply.tags["result"]             = "ok";
        reply.tags["ics_host"]           = ics_host_;
        reply.tags["ics_port"]           = port_s;
        reply.tags["ics_endpoint"]       = ics_host_ + ":" + port_s;
        reply.tags["ics_address"]        = ics_host_ + ":" + port_s;
        reply.tags["connection_address"] = ics_host_ + ":" + port_s;
        reply.tags["connection_host"]    = ics_host_;
        reply.tags["connection_port"]    = port_s;

        send_reply(session_, reply);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Members
    // ─────────────────────────────────────────────────────────────────────────

    SOCKET              socket_   = INVALID_SOCKET;
    uint16_t            port_     = 0;
    std::atomic<bool>   running_  = false;
    std::thread         thread_;

    TetherSession       session_;          // single-session server (one client)

    std::string         ics_host_ = "127.0.0.1";
    uint16_t            ics_port_ = 443;
};

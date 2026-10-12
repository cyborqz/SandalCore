// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once
/// ============================================================================
/// ProxyLogger — Structured JSONL file logger for proxy captures
/// ============================================================================
///
/// Writes one JSON object per line (JSONL) so it's trivially parseable with
/// Python, jq, or any other tool.
///
/// Each entry contains:
///   ts        — ISO-8601 timestamp with milliseconds
///   service   — "auth", "tether", "xclient"
///   dir       — "C>S" (client→server) or "S>C" (server→client)
///   seq       — per-session sequence number
///   type      — message_type_name from the MessageWrapper
///   status    — status_code (int)
///   sys_tags  — system_data.tags as a flat object
///   inner_b   — inner message_data size in bytes
///   wire_b    — full serialized wrapper size in bytes
///   inner_hex — hex dump of message_data  (capped at max_hex_bytes)
///   wire_hex  — hex dump of full wrapper  (capped at max_hex_bytes)
///   decoded   — best-effort decoded fields (varies by message type)
///

#include <nlohmann/json.hpp>
#include <fstream>
#include <mutex>
#include <string>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <spdlog/spdlog.h>

#include "ics_common.pb.h"
#include "ics_xclient.pb.h"

class ProxyLogger {
public:
    // ── Singleton ────────────────────────────────────────────────────────
    static ProxyLogger& instance() {
        static ProxyLogger inst;
        return inst;
    }

    /// Open (or reopen) the log file.  Appends by default.
    void open(const std::string& path) {
        std::lock_guard<std::mutex> lock(mu_);
        if (file_.is_open()) file_.close();
        file_.open(path, std::ios::app | std::ios::binary);
        if (!file_.is_open()) {
            spdlog::error("[ProxyLogger] Failed to open '{}'", path);
        } else {
            spdlog::info("[ProxyLogger] Structured log → {}", path);
            // Write a session separator
            nlohmann::json hdr;
            hdr["event"]   = "session_start";
            hdr["ts"]      = now_iso();
            file_ << hdr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
            file_.flush();
        }
    }

    bool is_open() const { return file_.is_open(); }

    /// Set max hex bytes to dump per field (default 32KB — plenty for analysis)
    void set_max_hex(size_t bytes) { max_hex_bytes_ = bytes; }
// (C.Y.B : CoA)

    // ── Main entry point ─────────────────────────────────────────────────
    /// Log a MessageWrapper flowing through the proxy.
    void log(const std::string& service,
             const std::string& direction,   // "C>S" or "S>C"
             int seq,
             const ics_common::MessageWrapper& msg)
    {
        if (!file_.is_open()) return;

        nlohmann::json j;
        j["ts"]      = now_iso();
        j["service"] = service;
        j["dir"]     = direction;
        j["seq"]     = seq;
        j["type"]    = msg.message_type_name();
        j["status"]  = static_cast<int>(msg.status_code());

        // system_data tags
        if (msg.has_system_data()) {
            nlohmann::json tags = nlohmann::json::object();
            for (const auto& kv : msg.system_data().tags())
                tags[kv.first] = kv.second;
            j["sys_tags"] = std::move(tags);
        }

        // sizes
        std::string wire = msg.SerializeAsString();
        j["inner_b"] = msg.message_data().size();
        j["wire_b"]  = wire.size();

        // hex dumps
        j["inner_hex"] = to_hex(msg.message_data(), max_hex_bytes_);
        j["wire_hex"]  = to_hex(wire, max_hex_bytes_);

        // best-effort decode of known inner types
        j["decoded"] = try_decode(msg);

        // Write atomically (one line = one JSON object)
        std::lock_guard<std::mutex> lock(mu_);
        file_ << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        file_.flush();   // flush every entry for crash-safety
    }

    /// Log a raw UDP packet (for GameServer captures)
    void log_udp(const std::string& direction,
                 const std::string& remote_addr,
                 const uint8_t* data, size_t len,
                 const std::string& description = "")
    {
        if (!file_.is_open()) return;

        nlohmann::json j;
        j["ts"]      = now_iso();
        j["service"] = "game_udp";
        j["dir"]     = direction;
        j["remote"]  = remote_addr;
        j["size"]    = len;
        j["desc"]    = description;
        j["hex"]     = to_hex(data, len, max_hex_bytes_);

        // Extract header fields for easy filtering
        if (len >= 4) {
            j["hdr"] = {
                {"b0", data[0]}, {"b1", data[1]},
                {"b2", data[2]}, {"b3", data[3]},
                {"last", data[len - 1]}
            };
        }

        std::lock_guard<std::mutex> lock(mu_);
        file_ << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        file_.flush();
    }

    /// Log an arbitrary event (session open/close, errors, etc.)
    void log_event(const std::string& service,
                   const std::string& event,
                   const nlohmann::json& extra = {})
    {
        if (!file_.is_open()) return;

        nlohmann::json j;
        j["ts"]      = now_iso();
        j["service"] = service;
        j["event"]   = event;
        if (!extra.is_null() && !extra.empty())
            j["data"] = extra;

        std::lock_guard<std::mutex> lock(mu_);
        file_ << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        file_.flush();
    }

private:
    ProxyLogger() = default;
    ProxyLogger(const ProxyLogger&) = delete;
    ProxyLogger& operator=(const ProxyLogger&) = delete;
// [C-O-A - C Y B]

    std::ofstream file_;
    std::mutex    mu_;
    size_t        max_hex_bytes_ = 32768;  // 32 KB per field

    // ── Helpers ──────────────────────────────────────────────────────────

    static std::string now_iso() {
        auto now = std::chrono::system_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
            << '.' << std::setw(3) << std::setfill('0') << ms.count();
        return oss.str();
    }

    static std::string to_hex(const std::string& data, size_t max_bytes) {
        return to_hex(reinterpret_cast<const uint8_t*>(data.data()),
                      data.size(), max_bytes);
    }

    static std::string to_hex(const uint8_t* data, size_t len, size_t max_bytes) {
        std::ostringstream oss;
        size_t show = std::min(len, max_bytes);
        for (size_t i = 0; i < show; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>(data[i]);
        }
        if (len > max_bytes)
            oss << "...(" << len << "B)";
        return oss.str();
    }

    // ── Known-type decoder ───────────────────────────────────────────────
    // Returns a JSON object with decoded fields for known message types.
    // This makes grepping the JSONL trivially easy.

    static nlohmann::json try_decode(const ics_common::MessageWrapper& msg) {
        const auto& t = msg.message_type_name();
        const auto& d = msg.message_data();
        nlohmann::json dec;

        try {
            if (t == "ics_common.SessionRequest") {
                ics_common::SessionRequest m;
                if (m.ParseFromString(d)) {
                    dec["request_type"] = static_cast<int>(m.request_type());
                    dec["tags"] = json_map(m.tags());
                    dec["data_map"] = json_map(m.data_map());
                    if (!m.data_string().empty()) dec["data_string"] = m.data_string();
                }
            }
            else if (t == "ics_common.SessionReply") {
                ics_common::SessionReply m;
                if (m.ParseFromString(d)) {
                    dec["result_code"] = static_cast<int>(m.result_code());
                    dec["request_type"] = static_cast<int>(m.request_type());
                    dec["tags"] = json_map(m.tags());
                    dec["data_map_keys"] = json_map_keys(m.data_map());
                    // data_map values can be huge (player_session_state binary),
                    // store sizes instead
                    for (const auto& kv : m.data_map())
                        dec["data_map_sizes"][kv.first] = kv.second.size();
                }
            }
            else if (t == "ics_common.KeepAliveMessage") {
                ics_common::KeepAliveMessage m;
                if (m.ParseFromString(d))
                    dec["timestamp_msec"] = m.timestamp_msec();
            }
            else if (t == "ics_xclient.CheckGameVersionRequest") {
                ics_xclient::CheckGameVersionRequest m;
                if (m.ParseFromString(d))
                    dec["game_version"] = m.game_version();
            }
            else if (t == "ics_xclient.CheckGameVersionReply") {
                ics_xclient::CheckGameVersionReply m;
                if (m.ParseFromString(d)) {
                    dec["supported"] = m.supported();
                    if (!m.message().empty()) dec["message"] = m.message();
                }
            }
            else if (t == "ics_xclient.GetWorldsReply") {
                ics_xclient::GetWorldsReply m;
                if (m.ParseFromString(d)) {
                    dec["world_count"] = m.worlds_size();
                    auto& arr = dec["worlds"] = nlohmann::json::array();
                    for (int i = 0; i < m.worlds_size(); ++i) {
                        auto& w = m.worlds(i);
                        arr.push_back({
                            {"id", w.world_id()},
                            {"name", w.world_display_name()},
                            {"available", w.available()},
                            {"fullness", w.fullness()},
                            {"queue", w.queue_job_count_hi()},
                            {"chars", w.character_count()}
                        });
                    }
                }
            }
            else if (t == "ics_xclient.WorldStatusEvent") {
                ics_xclient::WorldStatusEvent m;
                if (m.ParseFromString(d)) {
                    dec["world_count"] = m.worlds_size();
                    auto& arr = dec["worlds"] = nlohmann::json::array();
                    for (int i = 0; i < m.worlds_size(); ++i) {
                        auto& w = m.worlds(i);
                        arr.push_back({
                            {"id", w.world_id()},
                            {"name", w.world_display_name()},
                            {"available", w.available()},
                            {"fullness", w.fullness()},
                            {"queue", w.queue_job_count_hi()},
                            {"chars", w.character_count()}
                        });
                    }
                }
            }
            else if (t == "ics_xclient.GameClientInterestEvent") {
                ics_xclient::GameClientInterestEvent m;
                if (m.ParseFromString(d)) {
                    dec["start_mask"] = m.start_interest_bitmask();
                    dec["stop_mask"]  = m.stop_interest_bitmask();
                }
            }
            else if (t == "ics_xclient.GetCharactersReply") {
                ics_xclient::GetCharactersReply m;
                if (m.ParseFromString(d)) {
                    dec["char_count"] = m.characters_raw_size();
                    auto& arr = dec["characters"] = nlohmann::json::array();
                    for (int i = 0; i < m.characters_raw_size(); ++i) {
                        nlohmann::json ch;
                        ch["index"] = i;
                        ch["raw_size"] = m.characters_raw(i).size();
                        ch["raw_hex"] = to_hex(m.characters_raw(i), 32768);
                        // Decode top-level proto fields
                        ch["fields"] = decode_raw_fields(m.characters_raw(i));
                        arr.push_back(std::move(ch));
                    }
                }
            }
            else if (t == "ics_xclient.CreateCharacterRequest") {
                ics_xclient::CreateCharacterRequest m;
                if (m.ParseFromString(d)) {
                    dec["name"] = m.character_name();
                    dec["world"] = m.world_id();
                    dec["info_raw_size"] = m.create_character_info_raw().size();
                    dec["info_raw_hex"] = to_hex(m.create_character_info_raw(), 32768);
                    dec["info_fields"] = decode_raw_fields(m.create_character_info_raw());
                }
            }
            else if (t == "ics_xclient.CreateCharacterReply") {
                ics_xclient::CreateCharacterReply m;
                if (m.ParseFromString(d)) {
                    dec["char_id"] = m.character_id();
                    dec["name"] = m.character_name();
                    dec["world"] = m.world_id();
                    dec["info_raw_size"] = m.character_info_raw().size();
                    dec["info_raw_hex"] = to_hex(m.character_info_raw(), 32768);
                    dec["info_fields"] = decode_raw_fields(m.character_info_raw());
                }
            }
            else if (t == "ics_xclient.CheckCharacterNameRequest") {
                ics_xclient::CheckCharacterNameRequest m;
                if (m.ParseFromString(d)) {
                    dec["name"] = m.character_name();
                    dec["world"] = m.world_id();
                }
            }
            else if (t == "ics_xclient.CheckCharacterNameReply") {
                ics_xclient::CheckCharacterNameReply m;
                if (m.ParseFromString(d)) {
                    dec["name"] = m.character_name();
                    dec["available"] = m.available();
                }
            }
            else if (t == "ics_xclient.SelectCharacterRequest") {
                ics_xclient::SelectCharacterRequest m;
                if (m.ParseFromString(d))
                    dec["char_id"] = m.character_id();
            }
            else if (t == "ics_xclient.SelectCharacterReply") {
                ics_xclient::SelectCharacterReply m;
                if (m.ParseFromString(d))
                    dec["char_id"] = m.character_id();
            }
            else if (t == "ics_xclient.PlayRequest") {
                ics_xclient::PlayRequest m;
                if (m.ParseFromString(d)) {
                    dec["char_id"] = m.character_id();
                    dec["world"] = m.world_id();
                }
            }
            else if (t == "ics_xclient.PlayReply") {
                ics_xclient::PlayReply m;
                if (m.ParseFromString(d)) {
                    if (!m.game_connection_token().empty())
                        dec["token"] = m.game_connection_token();
                    if (!m.game_connection_string().empty())
                        dec["conn"] = m.game_connection_string();
                }
            }
            else if (t == "ics_xclient.PlayRequestStatusReport") {
                ics_xclient::PlayRequestStatusReport m;
                if (m.ParseFromString(d))
                    dec["queue_position"] = m.queue_position();
            }
            else if (t == "ics_xclient.PlayerSessionState") {
                ics_xclient::PlayerSessionState m;
                if (m.ParseFromString(d)) {
                    dec["char_id"] = m.character_id();
                    dec["world"] = m.world_id();
                    dec["pending"] = m.has_pending_play_request();
                    dec["mini_count"] = m.character_mini_records_size();
                    auto& arr = dec["mini_records"] = nlohmann::json::array();
                    for (int i = 0; i < m.character_mini_records_size(); ++i) {
                        auto& r = m.character_mini_records(i);
                        arr.push_back({
                            {"id", r.character_id()},
                            {"name", r.character_name()},
                            {"world", r.world_id()}
                        });
                    }
                }
            }
            else if (t == "ics_xclient.AuthRefreshRequest") {
                ics_xclient::AuthRefreshRequest m;
                if (m.ParseFromString(d)) {
                    dec["auth_token_len"] = m.auth_token().size();
                    dec["refresh_token_len"] = m.refresh_token().size();
                }
            }
            else if (t == "ics_xclient.AuthRefreshReply") {
                ics_xclient::AuthRefreshReply m;
                if (m.ParseFromString(d)) {
                    dec["auth_token_len"] = m.auth_token().size();
                    dec["refresh_token_len"] = m.refresh_token().size();
                }
            }
            else if (t == "ics_xclient.DeleteCharacterRequest") {
                ics_xclient::DeleteCharacterRequest m;
                if (m.ParseFromString(d)) {
                    dec["char_id"] = m.character_id();
                    dec["name"] = m.character_name();
                    dec["world"] = m.world_id();
                }
            }
            else if (t == "ics_xclient.CanDeleteCharacterRequest") {
                ics_xclient::CanDeleteCharacterRequest m;
                if (m.ParseFromString(d)) {
                    dec["char_id"] = m.character_id();
                    dec["world"] = m.world_id();
                }
            }
            else if (t == "ics_xclient.CanDeleteCharacterReply") {
                ics_xclient::CanDeleteCharacterReply m;
                if (m.ParseFromString(d)) {
                    dec["char_id"] = m.character_id();
                    dec["can_delete"] = m.can_delete();
                }
            }
            else if (t == "ics_xclient.GetShopHandoffCodeReply") {
                ics_xclient::GetShopHandoffCodeReply m;
                if (m.ParseFromString(d))
                    dec["code"] = m.shop_handoff_code();
            }
            // Add more types as needed — unknown types get empty decoded
        }
        catch (const std::exception& e) {
            dec["decode_error"] = e.what();
        }
// ~ C | Y | B & CoA ~

        return dec;
    }

    // ── Raw protobuf field decoder → JSON ────────────────────────────────
    // Produces a JSON array of {field, wire, value} for any arbitrary proto.

    static nlohmann::json decode_raw_fields(const std::string& data, int max_depth = 3) {
        auto arr = nlohmann::json::array();
        if (data.empty() || max_depth <= 0) return arr;

        size_t pos = 0;
        auto read_varint = [&](uint64_t& out) -> bool {
            out = 0; int shift = 0;
            while (pos < data.size()) {
                uint8_t b = static_cast<uint8_t>(data[pos++]);
                out |= static_cast<uint64_t>(b & 0x7f) << shift;
                shift += 7;
                if (!(b & 0x80)) return true;
                if (shift >= 64) return false;
            }
            return false;
        };

        while (pos < data.size()) {
            uint64_t tag;
            if (!read_varint(tag)) break;
            uint32_t fn = static_cast<uint32_t>(tag >> 3);
            uint32_t wt = static_cast<uint32_t>(tag & 7);
            if (fn == 0) break;

            nlohmann::json f;
            f["f"] = fn;

            switch (wt) {
            case 0: {
                uint64_t v;
                if (!read_varint(v)) return arr;
                f["w"] = "varint";
                f["v"] = v;
                break;
            }
            case 1: {
                if (pos + 8 > data.size()) return arr;
                uint64_t v = 0;
                std::memcpy(&v, &data[pos], 8); pos += 8;
                f["w"] = "fixed64";
                f["v"] = v;
                break;
            }
            case 2: {
                uint64_t len;
                if (!read_varint(len) || pos + len > data.size()) return arr;
                std::string bytes = data.substr(pos, static_cast<size_t>(len));
                pos += static_cast<size_t>(len);
                f["w"] = "bytes";
                f["len"] = bytes.size();

                // Try to detect text vs binary
                bool is_text = bytes.size() > 0 && bytes.size() < 4096;
                if (is_text) {
                    for (unsigned char c : bytes) {
                        if (c < 32 && c != '\n' && c != '\r' && c != '\t') { is_text = false; break; }
                        if (c > 126) { is_text = false; break; }
                    }
                }
                if (is_text) {
                    f["text"] = bytes;
                } else {
                    f["hex"] = to_hex(bytes, 1024);
                    // Try recursing into sub-message
                    if (bytes.size() >= 2 && max_depth > 1) {
                        auto sub = decode_raw_fields(bytes, max_depth - 1);
                        if (!sub.empty() && sub.is_array() && sub.size() > 0)
                            f["sub"] = std::move(sub);
                    }
                }
                break;
            }
            case 5: {
                if (pos + 4 > data.size()) return arr;
                uint32_t v = 0;
                std::memcpy(&v, &data[pos], 4); pos += 4;
                f["w"] = "fixed32";
                f["v"] = v;
                break;
            }
            default:
                return arr;  // unknown wire type → stop
            }

            arr.push_back(std::move(f));
        }
        return arr;
    }

    // Map<string,string> → JSON object
    template <typename MapT>
    static nlohmann::json json_map(const MapT& m) {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& kv : m) j[kv.first] = kv.second;
        return j;
    }

    // Map keys only (for large maps where values are binary)
    template <typename MapT>
    static nlohmann::json json_map_keys(const MapT& m) {
        auto arr = nlohmann::json::array();
        for (const auto& kv : m) arr.push_back(kv.first);
        return arr;
    }
};

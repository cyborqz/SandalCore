// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  services/tether/ics_token_file.h
//
//  The login-token hand-off between launcher.exe and tether_server.exe. After
//  a successful login the launcher saves the player's token (the auth
//  service's access_token) here; the tether gives it to the game client as
//  its ICS auth_token, and the Go gateway resolves it to the account. Both
//  executables run from dist\<Config>, so the file lives next to them:
//  data\ics_auth_token.txt.
//
//  The tether reads the file only when started with --login-token-file (the
//  Go stack, scripts\launch_go.bat), and hands the saved token only to a game
//  client on this machine (client_tokens). Without the flag it sends the
//  emulator token, which the legacy aoc_server's own ICS accepts.
// ============================================================================
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

namespace aoc { namespace ics_token {

/// data\ics_auth_token.txt under exe_dir (which ends with a separator).
inline std::string path_in(const std::string& exe_dir) {
    return exe_dir + "data\\ics_auth_token.txt";
}

/// Lets only the current user open `path` (ics_token_acl.cpp: a protected
/// one-entry DACL on Windows, mode 0600 elsewhere).  False if it could not.
bool restrict_to_owner(const std::string& path);

/// True if only the current user may open `path`.
bool is_owner_only(const std::string& path);

/// How often write() retries replacing the file, and how long it waits
/// between tries: the tether may have it open for a moment.
inline constexpr int kReplaceRetries = 5;
inline constexpr std::chrono::milliseconds kReplaceRetryDelay{50};
// built with CoA and C-Y-B

/// Replaces the file's content with token, creating the data directory. The
/// token goes to <path>.tmp first, which then replaces the file, so a reader
/// sees the old token or the new one, never a half-written file. False when
/// the file cannot be written or replaced; the temp file is removed then.
inline bool write(const std::string& path, const std::string& token) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // A failure here shows up as the temp file failing to open.
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << token;
        f.flush();
        f.close();
        if (!f) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    // Only the signed-in user may read the token.  Best effort: a filesystem
    // without ACLs must not stop the login.  The rename keeps the ACL.
    restrict_to_owner(tmp);
    // std::filesystem::rename is MoveFileExW with MOVEFILE_REPLACE_EXISTING.
    for (int retry = 0;; ++retry) {
        fs::rename(tmp, path, ec);
        if (!ec) return true;
        if (retry == kReplaceRetries) break;
        std::this_thread::sleep_for(kReplaceRetryDelay);
    }
    fs::remove(tmp, ec);
    return false;
}

/// A login token is a few dozen bytes; a bigger file is not a token file.
inline constexpr std::size_t kMaxFileBytes = 4096;

/// The saved token without surrounding whitespace (and without the UTF-8 BOM
/// that Notepad and PowerShell 5.1 add). "" when the file is missing, blank,
/// larger than kMaxFileBytes, or holds anything but printable ASCII
/// (0x21-0x7E) once trimmed: UTF-16, binary data, several lines.
inline std::string read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s(kMaxFileBytes + 1, '\0');
    f.read(s.data(), static_cast<std::streamsize>(s.size()));
    s.resize(static_cast<std::size_t>(f.gcount()));
    if (s.size() > kMaxFileBytes) return {};
    if (s.rfind("\xEF\xBB\xBF", 0) == 0) s.erase(0, 3);
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, last - first + 1);
    for (const unsigned char c : s)
        if (c < 0x21 || c > 0x7E) return {};
    return s;
}

/// True for 127.0.0.0/8; addr in host byte order (ntohl of sin_addr).
inline bool is_loopback_ipv4(std::uint32_t addr) {
    return (addr >> 24) == 127;
}

struct ClientTokens { std::string auth, refresh; bool saved; };

/// What the tether hands the game client as its ICS auth and refresh token:
/// the saved login token when saved tokens are enabled (token_file is not
/// empty), the client is on this machine and the file holds a token;
/// otherwise the emulator's fixed pair. The file is not touched unless the
/// first two hold.
inline ClientTokens client_tokens(const std::string& token_file, bool peer_is_loopback) {
    const ClientTokens emulator{"emulator-ics-token-deadbeef", "emulator-refresh-deadbeef", false};
    if (token_file.empty() || !peer_is_loopback) return emulator;
    const std::string saved = read(token_file);
    if (saved.empty()) return emulator;
    return {saved, saved, true};
}

/// The auth service's default token lifetime (AOC_TOKEN_TTL): a file older
/// than this most likely holds an expired token.
inline constexpr std::chrono::hours kDefaultTokenLifetime{24};

/// Time since the file was last written; nullopt when it cannot be told.
inline std::optional<std::chrono::seconds> age(const std::string& path) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) return std::nullopt;
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::filesystem::file_time_type::clock::now() - written);
}

/// "3h 12m" (hours are not folded into days); a negative age reads as 0h 0m.
inline std::string format_age(std::chrono::seconds age) {
    const long long minutes = (age.count() < 0 ? 0 : age.count()) / 60;
    return std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m";
}

}}  // namespace aoc::ics_token

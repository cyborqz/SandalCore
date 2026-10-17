// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  launcher_sign_in.h
//
//  "Keep me signed in" for launcher.exe (src/launcher_main.cpp):
//
//   * the saved sign-in in Windows Credential Manager: one generic credential
//     per Windows user, under ONE target name (kCredentialTarget) that every
//     copy of the launcher shares: user name = the account name, secret = the
//     password.  The auth service's login token is not saved instead: it lives
//     24 hours (AOC_TOKEN_TTL), there is no refresh token, and each auth
//     service checks it in its own database.  Nothing is written to a file, a
//     log or the registry;
//   * the one-time move of what an earlier version saved under other names to
//     the current names: a sign-in (credential_move) and the remembered user
//     name file (copy_if_missing).  The old names are not in this file;
//   * where the auth server is (config\server.json) and the hosts the saved
//     password may go to: exactly 127.0.0.1, localhost or ::1, read once per
//     automatic sign-in and used for every retry;
//   * how a request is written (the password is wiped after it is sent) and how
//     a reply of the auth server is read: by length, at most 64 KB, inside one
//     deadline, a redirect never followed; then what it means (token / refused /
//     not ready);
//   * the automatic sign-in at start, written against injected operations (a
//     clock, a sleep, the request, the token file) so test_launcher_saved_sign_in
//     runs it with a fake clock and a fake auth server.  The window runs it on
//     a worker thread;
//   * LAUNCH GAME: the game client's command line and folder from
//     config\server.json, and the check for a client that already runs;
//   * reading this PC: the launcher's own folder (a path of any length) and a
//     text value of the registry (always ended by a terminator).
//
//  The sentences the window shows the player are plain words: no file names,
//  ports, tokens or server names (the test checks that).  Only text in ASCII
//  here (the window turns the strings into wide ones).
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wincred.h>

#pragma comment(lib, "advapi32.lib")

#include <nlohmann/json.hpp>
// [C_Y_B ~ CoA]

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace aoc { namespace launcher {

// ---------------------------------------------------------------------------
//  The saved sign-in (Windows Credential Manager)
// ---------------------------------------------------------------------------

/// The one target name every copy of the launcher uses.
inline constexpr wchar_t kCredentialTarget[] = L"SandalCore/LauncherSignIn";

/// Account name and password as the sign-in form holds them (UTF-16).  The
/// password is overwritten when the object goes away (best effort).
struct SavedSignIn {
    std::wstring username;
    std::wstring password;

    SavedSignIn() = default;
    SavedSignIn(std::wstring u, std::wstring p) : username(std::move(u)), password(std::move(p)) {}
    SavedSignIn(const SavedSignIn&) = default;
    SavedSignIn(SavedSignIn&&) noexcept = default;
    SavedSignIn& operator=(const SavedSignIn&) = default;
    SavedSignIn& operator=(SavedSignIn&&) noexcept = default;
    ~SavedSignIn() {
        if (!password.empty()) SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    }
};

/// Saves s under `target` (replacing what is there), kept for this Windows user
/// on this PC (CRED_PERSIST_LOCAL_MACHINE).  False when the user name or the
/// password is empty or Windows refuses.
inline bool credential_save(const wchar_t* target, const SavedSignIn& s) {
    if (!target || !*target || s.username.empty() || s.password.empty()) return false;
    const std::size_t bytes = s.password.size() * sizeof(wchar_t);
    if (bytes > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
    static wchar_t comment[] =
        L"Saved by the SandalCore launcher (Keep me signed in). Switch Account / Logout deletes it.";
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = const_cast<LPWSTR>(target);
    c.Comment = comment;
    c.CredentialBlobSize = static_cast<DWORD>(bytes);
    c.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(s.password.data()));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    c.UserName = const_cast<LPWSTR>(s.username.c_str());
    return CredWriteW(&c, 0) == TRUE;
}

/// What is saved under `target`; nullopt when nothing is, or what is there is
/// not a user name with a UTF-16 password.
inline std::optional<SavedSignIn> credential_load(const wchar_t* target) {
    if (!target || !*target) return std::nullopt;
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &c) || !c) return std::nullopt;
    std::optional<SavedSignIn> out;
    if (c->UserName && *c->UserName && c->CredentialBlob &&
        c->CredentialBlobSize >= sizeof(wchar_t) && c->CredentialBlobSize % sizeof(wchar_t) == 0) {
        SavedSignIn s;
        s.username = c->UserName;
        s.password.assign(reinterpret_cast<const wchar_t*>(c->CredentialBlob),
                          c->CredentialBlobSize / sizeof(wchar_t));
        out = std::move(s);
    }
    if (c->CredentialBlob && c->CredentialBlobSize) SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return out;
}

/// Deletes what is saved under `target`.  True when it is gone: deleted now, or
/// there was nothing.
inline bool credential_remove(const wchar_t* target) {
    if (!target || !*target) return false;
    if (CredDeleteW(target, CRED_TYPE_GENERIC, 0)) return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

/// True when an entry of any kind is saved under `target`.  What is in it is not
/// kept: the secret is wiped as soon as Windows has handed it over.
inline bool credential_exists(const wchar_t* target) {
    if (!target || !*target) return false;
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &c) || !c) return false;
    if (c->CredentialBlob && c->CredentialBlobSize) SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return true;
}

/// How the move of a saved sign-in to another target name ended.
enum class Moved {
    Nothing,  ///< nothing to do: no sign-in under the old name, an entry under the new name already, or the same name
    Done,     ///< the sign-in is saved under the new name, and the old entry is deleted
    Failed,   ///< Windows refused the new entry (the old one is untouched) or the delete of the old one (both exist)
};
// [CoA - C | Y | B]

/// Moves a sign-in saved under `old_target` to `new_target`, once: only when the
/// new name holds nothing yet and the old one holds a sign-in.  The old entry is
/// deleted only after the new one is saved.  The password is neither printed nor
/// logged; the copies in memory are wiped.
inline Moved credential_move(const wchar_t* old_target, const wchar_t* new_target) {
    if (!old_target || !*old_target || !new_target || !*new_target) return Moved::Nothing;
    // Target names do not tell capital letters apart: the same name in other letters is the same entry.
    if (CompareStringOrdinal(old_target, -1, new_target, -1, TRUE) == CSTR_EQUAL) return Moved::Nothing;
    if (credential_exists(new_target)) return Moved::Nothing;
    const std::optional<SavedSignIn> old = credential_load(old_target);
    if (!old) return Moved::Nothing;
    if (!credential_save(new_target, *old)) return Moved::Failed;
    return credential_remove(old_target) ? Moved::Done : Moved::Failed;
}

/// Copies the file `from` to `to`, only when `to` is not there and `from` is a
/// file; makes the folder of `to`.  `from` stays.  True when a copy was made.
/// Never throws.
inline bool copy_if_missing(const std::filesystem::path& from, const std::filesystem::path& to) {
    namespace fs = std::filesystem;
    std::error_code ec;
    try {
        if (from.empty() || to.empty()) return false;
        if (!fs::is_regular_file(from, ec) || ec) return false;
        if (fs::exists(to, ec) || ec) return false;        // there already, or cannot tell: leave it
        fs::create_directories(to.parent_path(), ec);
        if (ec) return false;
        fs::path tmp = to;
        tmp += L".tmp";
        fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
        fs::rename(tmp, to, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
        return true;
    } catch (...) { return false; }
}

/// What a finished sign-in does to the saved sign-in.
enum class Remember {
    Save,    ///< replace it with the sign-in that just worked
    Delete,  ///< forget it
    Leave,   ///< keep it as it is
};

/// By hand and it worked: save it when "Keep me signed in" is ticked, forget it
/// when it is not.  By hand and it failed (a typo), or automatic whatever came
/// of it (signed in, refused, cancelled, no server): leave it alone.
/// Switch Account / Logout is not a sign-in: it always deletes.
inline Remember remember_action(bool automatic, bool signed_in, bool keep_ticked) {
    if (automatic || !signed_in) return Remember::Leave;
    return keep_ticked ? Remember::Save : Remember::Delete;
}

/// True only for the three names of this PC, written exactly like this:
/// "127.0.0.1", "localhost" (any case) and "::1" or "[::1]".  The saved password
/// is only sent on its own to an auth server on this PC.  Everything else is
/// refused, however it reads: another number (127.1.2.3, 0.0.0.0), a spelling a
/// parser could read as something else (0127.0.0.1, 127.1), a name that only
/// starts like one (127.0.0.1.example.com, localhost.), a port, a blank or any
/// other character before, inside or after it.
inline bool is_local_host(const std::string& host) {
    if (host == "127.0.0.1" || host == "::1" || host == "[::1]") return true;
    static const char kLocalhost[] = "localhost";
    if (host.size() != sizeof(kLocalhost) - 1) return false;
    for (std::size_t i = 0; i < host.size(); ++i) {
        char ch = host[i];
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));   // ASCII only, whatever the locale
        if (ch != kLocalhost[i]) return false;
    }
    return true;
}
// built with C Y B and CoA

// ---------------------------------------------------------------------------
//  Where the auth server is (config\server.json)
// ---------------------------------------------------------------------------

/// The auth server this run talks to: auth_server_host and auth_server_port of
/// config\server.json (the start script puts its own copy next to the exe).
struct AuthEndpoint {
    std::string host = "127.0.0.1";
    int port = 8081;
};

/// A key that is missing or of the wrong type keeps its default; a port outside
/// 1-65535 too.  The host is taken as written (an empty one is not this PC).
inline AuthEndpoint auth_endpoint(const nlohmann::json& server_json) {
    AuthEndpoint e;
    if (!server_json.is_object()) return e;
    const auto host = server_json.find("auth_server_host");
    if (host != server_json.end() && host->is_string()) e.host = host->get<std::string>();
    const auto port = server_json.find("auth_server_port");
    if (port != server_json.end() && port->is_number_integer()) {
        const long long p = port->get<long long>();
        if (p > 0 && p < 65536) e.port = static_cast<int>(p);
    }
    return e;
}

/// The server an automatic sign-in will use, or nothing when it is not on this
/// PC.  The caller reads config\server.json ONCE for this and keeps the answer
/// for the whole sign-in, every retry included: the host that was checked is
/// the host that gets the password, even if the file changes meanwhile.
inline std::optional<AuthEndpoint> auto_sign_in_endpoint(const nlohmann::json& server_json) {
    AuthEndpoint e = auth_endpoint(server_json);
    if (!is_local_host(e.host)) return std::nullopt;
    return e;
}

// ---------------------------------------------------------------------------
//  Sentences the window shows the player when a sign-in does not work
// ---------------------------------------------------------------------------

inline constexpr const char* kMsgCannotReach  = "Can't reach the sign-in server. Is the server running?";
inline constexpr const char* kMsgUnexpected   = "The sign-in server sent an unexpected reply.";
inline constexpr const char* kMsgServerTrouble = "The sign-in server had a problem. Please try again.";
inline constexpr const char* kMsgSignInFailed = "Sign-in failed.";
inline constexpr const char* kMsgNotPassedOn  = "Signed in, but your sign-in could not be passed to the game. Please try again.";
inline constexpr const char* kMsgNotAnswering = "The sign-in server isn't answering.";

// ---------------------------------------------------------------------------
//  What the auth server answered
// ---------------------------------------------------------------------------

enum class ReplyKind {
    Token,     ///< signed in: `token` is the session token
    Refused,   ///< the server said no (wrong password, unknown account, bad input): do not ask again
    NotReady,  ///< no answer, or "server_error" (its database is not up yet): ask again later
};

struct Reply {
    ReplyKind kind = ReplyKind::NotReady;
    std::string token;
    std::string message;   ///< the sentence for the player (empty for a token)
};

inline std::string json_string_field(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

/// Reads the body of an auth server answer (POST /api/v1/auth/login or /register).
inline Reply parse_reply(const std::string& body) {
    Reply r;
    if (body.empty()) {                                   // nothing listens, or it hung up
        r.kind = ReplyKind::NotReady;
        r.message = kMsgCannotReach;
        return r;
    }
    nlohmann::json j;
    // A NUL is no part of JSON, and the parser would stop reading at it and ignore the rest.
    if (body.find('\0') == std::string::npos) {
        try { j = nlohmann::json::parse(body); } catch (...) { j = nullptr; }
    }
    if (!j.is_object()) {
        r.kind = ReplyKind::Refused;
        r.message = kMsgUnexpected;
        return r;
    }
    if (j.contains("access_token")) {
        r.token = json_string_field(j, "access_token");
        if (r.token.empty()) {                            // accepted, but nothing to hand to the game
            r.kind = ReplyKind::Refused;
            r.message = kMsgUnexpected;
        } else {
            r.kind = ReplyKind::Token;
        }
        return r;
    }
    const std::string error = json_string_field(j, "error");
    const std::string message = json_string_field(j, "message");
    if (error == "server_error") {                        // the login service is up, its database is not (yet)
        r.kind = ReplyKind::NotReady;
        r.message = kMsgServerTrouble;
        return r;
    }
    r.kind = ReplyKind::Refused;
    r.message = !message.empty() ? message : kMsgSignInFailed;   // never a bare error code
    return r;
}

// ---------------------------------------------------------------------------
//  Sending the password, and reading the answer
// ---------------------------------------------------------------------------

using Ms = std::chrono::milliseconds;

/// Overwrites a secret in place, then empties the string.
inline void wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}
inline void wipe(std::wstring& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
    s.clear();
}
// C-O-A and C | Y | B

inline void append_json_string(std::string& out, const std::string& s) {
    static const char kHex[] = "0123456789abcdef";
    out += '"';
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (u < 0x20)         { out += "\\u00"; out += kHex[u >> 4]; out += kHex[u & 15]; }
        else                       { out += c; }   // UTF-8 goes through as it is
    }
    out += '"';
}

/// The body of a login or register request: {"username":...,"password":...}.
/// It is written straight into one string, with no JSON object in between: a
/// JSON library keeps its own copies of what it is given and of what it writes,
/// and they cannot be wiped.  The caller wipes the body after sending it.
inline std::string login_body(const std::string& user, const std::string& pass) {
    std::string body;
    // Room for the worst case (every character written as \u00xx), so the body is never
    // moved to a bigger buffer, which would leave a copy of the password behind.
    body.reserve((user.size() + pass.size()) * 6 + 48);
    body += "{\"username\":";
    append_json_string(body, user);
    body += ",\"password\":";
    append_json_string(body, pass);
    body += '}';
    return body;
}

/// Keeps a deadline for a call that cannot be given one itself.  When `at` passes,
/// `cancel` runs once, on this watch's own thread; it should make the blocked call
/// fail (for WinInet: close the request handle from here).  stop() ends the watch
/// and says whether the deadline came first.  Make one, run the call, then stop().
class DeadlineWatch {
public:
    DeadlineWatch(std::function<void()> cancel, std::chrono::steady_clock::time_point at)
        : thread_([this, cancel = std::move(cancel), at] {
              std::unique_lock<std::mutex> lock(mutex_);
              if (!cv_.wait_until(lock, at, [this] { return done_; })) {
                  fired_ = true;
                  if (cancel) cancel();
              }
          }) {}
    DeadlineWatch(const DeadlineWatch&) = delete;
    DeadlineWatch& operator=(const DeadlineWatch&) = delete;
    ~DeadlineWatch() { stop(); }

    /// True when the deadline passed before this call (and `cancel` ran).
    bool stop() {
        { std::lock_guard<std::mutex> lock(mutex_); done_ = true; }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        return fired_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    bool fired_ = false;
    std::thread thread_;   // last: it starts when everything above exists
};

/// A reply of the auth server is a few hundred bytes; nothing near this size is one.
inline constexpr std::size_t kMaxReplyBytes = 64 * 1024;

/// How reading one reply ended.
enum class ReadEnd {
    Complete,    ///< the server finished its reply
    TooBig,      ///< more than the allowed size arrived: not a reply of ours
    Redirected,  ///< the server answered with a redirect: never followed
    TimedOut,    ///< the deadline came first
    Failed,      ///< the connection broke, or never came up
};

struct ReadResult {
    ReadEnd end = ReadEnd::Failed;
    std::string body;   ///< the reply as bytes, by length (a NUL inside is data); empty unless Complete
};

/// Reads one reply to the end, within max_bytes and before deadline_at (on the
/// clock `now` reads).  read_some(buffer, size, time left) reads up to `size`
/// bytes, waiting at most the time left, and returns how many; 0 is the end of
/// the reply and a negative number an error.
inline ReadResult read_reply(const std::function<long(char*, std::size_t, Ms)>& read_some,
                             const std::function<Ms()>& now, Ms deadline_at,
                             std::size_t max_bytes = kMaxReplyBytes) {
    ReadResult out;
    char chunk[4096];
    for (;;) {
        const Ms left = deadline_at - now();
        if (left.count() <= 0) { out.end = ReadEnd::TimedOut; out.body.clear(); return out; }
        // never more than one byte past the limit: if that byte comes, the reply is too big
        const std::size_t want = std::min(sizeof(chunk), max_bytes - out.body.size() + 1);
        const long got = read_some(chunk, want, left);
        if (got < 0) { out.end = ReadEnd::Failed; out.body.clear(); return out; }
        if (got == 0) { out.end = ReadEnd::Complete; return out; }
        out.body.append(chunk, static_cast<std::size_t>(got));
        if (out.body.size() > max_bytes) { out.end = ReadEnd::TooBig; out.body.clear(); return out; }
    }
}

/// What a finished read means for the sign-in: a complete reply is read as the
/// auth server's answer; a reply that is too big or a redirect is refused as an
/// unexpected reply; no answer in time, or a broken connection, is "not ready".
inline Reply reply_from_read(const ReadResult& r) {
    if (r.end == ReadEnd::Complete) return parse_reply(r.body);
    if (r.end == ReadEnd::TooBig || r.end == ReadEnd::Redirected) {
        Reply x;
        x.kind = ReplyKind::Refused;
        x.message = kMsgUnexpected;
        return x;
    }
    return parse_reply("");
}

// ---------------------------------------------------------------------------
//  The automatic sign-in at start
// ---------------------------------------------------------------------------

struct AutoTiming {
    Ms start_delay{3000};     ///< the "Signing in as <name>..." moment, Cancel works
    Ms retry_every{2000};     ///< the auth server is not up yet: ask again this often
    Ms give_up_after{60000};  ///< ... for this long, counted from the first request
    Ms slice{100};            ///< Cancel is looked at this often
};

/// What auto_sign_in needs from its surroundings.
struct AutoOps {
    std::function<bool()> cancelled;                       ///< the player pressed Cancel (or closed the window)
    std::function<void(const std::string&)> show;          ///< the small status line under "Signing in as ..."
    std::function<void(Ms)> sleep;
    std::function<Ms()> now;                               ///< a steady clock
    std::function<Reply(const SavedSignIn&)> request;      ///< one login request
    std::function<bool(const std::string&)> store_token;   ///< writes data\ics_auth_token.txt
};

enum class AutoOutcome {
    SignedIn,       ///< the token file is written
    Cancelled,      ///< the player pressed Cancel; no token was written
    Refused,        ///< the server said no: `message` says what
    GaveUp,         ///< the auth server did not come up in time
    TokenNotSaved,  ///< the server accepted, but the token file could not be written
};
// (C O A | C-Y-B)

struct AutoResult {
    AutoOutcome outcome = AutoOutcome::Cancelled;
    std::string message;
    int attempts = 0;   ///< requests made
};

namespace detail {
/// Sleeps `total` in slices; false if Cancel came meanwhile.
inline bool pause(const AutoOps& ops, Ms total, Ms slice) {
    for (Ms done{0}; done < total; ) {
        if (ops.cancelled()) return false;
        const Ms step = std::min(slice, total - done);
        ops.sleep(step);
        done += step;
    }
    return !ops.cancelled();
}
inline std::string seconds(Ms ms) { return std::to_string(ms.count() / 1000); }
}  // namespace detail

/// Signs in with the saved sign-in: waits start_delay (Cancel works), then asks
/// the auth server, again every retry_every while it is not up yet, for up to
/// give_up_after.  A refusal ends it at once.  A token that arrives after Cancel
/// is dropped, never written.  Blocking: run it on a worker thread.
inline AutoResult auto_sign_in(const SavedSignIn& saved, const AutoOps& ops, const AutoTiming& t = {}) {
    AutoResult res;

    // 1. the "Signing in as <name>..." moment
    Ms waited{0};
    long long shown = -1;
    while (waited < t.start_delay) {
        if (ops.cancelled()) return res;
        const long long left = ((t.start_delay - waited).count() + 999) / 1000;
        if (left != shown) {
            shown = left;
            ops.show("Starting in " + std::to_string(left) + " s. Press Cancel to use another account.");
        }
        const Ms step = std::min(t.slice, t.start_delay - waited);
        ops.sleep(step);
        waited += step;
    }

    // 2. ask; while the auth server is not up yet, ask again
    const Ms began = ops.now();
    for (;;) {
        if (ops.cancelled()) return res;
        ++res.attempts;
        ops.show("Contacting the sign-in server...");
        const Reply r = ops.request(saved);
        if (ops.cancelled()) return res;                   // Cancel came while the request was out

        if (r.kind == ReplyKind::Token) {
            if (!ops.store_token(r.token)) {
                res.outcome = AutoOutcome::TokenNotSaved;
                res.message = kMsgNotPassedOn;
            } else {
                res.outcome = AutoOutcome::SignedIn;
            }
            return res;
        }
        if (r.kind == ReplyKind::Refused) {
            res.outcome = AutoOutcome::Refused;
            res.message = r.message;
            return res;
        }

        const Ms elapsed = ops.now() - began;
        if (elapsed >= t.give_up_after) {
            res.outcome = AutoOutcome::GaveUp;
            res.message = kMsgNotAnswering;
            return res;
        }
        ops.show("The sign-in server isn't ready yet. Trying again... (" + detail::seconds(elapsed) + " s)");
        if (!detail::pause(ops, t.retry_every, t.slice)) return res;
    }
}

// ---------------------------------------------------------------------------
//  LAUNCH GAME: starting the game client
// ---------------------------------------------------------------------------

/// What config\server.json says about starting the client: the tether port
/// (default 19021) and the extra arguments (-UseEOS=false switches Epic Online
/// Services off).
struct LaunchSettings {
    int tether_port = 19021;
    std::string client_args;
};

inline LaunchSettings launch_settings(const nlohmann::json& server_json) {
    LaunchSettings s;
    if (!server_json.is_object()) return s;
    const auto port = server_json.find("tether_port");
    if (port != server_json.end() && port->is_number_integer()) {
        const long long p = port->get<long long>();
        if (p > 0 && p < 65536) s.tether_port = static_cast<int>(p);
    }
    std::string args = json_string_field(server_json, "client_extra_args");
    const auto first = args.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) s.client_args = args.substr(first, args.find_last_not_of(" \t\r\n") - first + 1);
    return s;
}

/// What LAUNCH GAME runs: the client's command line and the folder it starts in.
/// The same as the launch script used to run:
///   "<game exe>" -LauncherTetherPort=<tether_port> <client_extra_args>
/// started in the client's own folder (<game root>\AOC\Binaries\Win64).
struct ClientCommand {
    std::string command_line;
    std::string working_dir;   ///< with the closing backslash; empty when exe has no folder
};

inline ClientCommand client_command(const std::string& exe, const LaunchSettings& s) {
    ClientCommand c;
    c.command_line = "\"" + exe + "\" -LauncherTetherPort=" + std::to_string(s.tether_port);
    if (!s.client_args.empty()) c.command_line += ' ' + s.client_args;
    const auto slash = exe.find_last_of("\\/");
    if (slash != std::string::npos) c.working_dir = exe.substr(0, slash + 1);
    return c;
}
// C.O.A / Cyb

/// True for the names of the game's programs: AOCClient.exe and
/// AOCClient-Win64-Shipping.exe (any case).  Not its crash reporter.
inline bool is_game_process_name(const std::wstring& file) {
    std::wstring n = file;
    for (wchar_t& ch : n) if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
    if (n == L"aocclient.exe") return true;
    return n.size() > 14 && n.compare(0, 10, L"aocclient-") == 0 && n.compare(n.size() - 4, 4, L".exe") == 0;
}

/// True when a game client runs on this PC (the process list is read: names only).
inline bool game_is_running() {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = is_game_process_name(e.szExeFile);
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------------------
//  Reading this PC: the launcher's own folder, the registry
// ---------------------------------------------------------------------------

/// The longest path Windows has, in characters, with its terminator.
inline constexpr std::size_t kMaxWidePath = 32768;

/// A path from a Windows call that fills a wide buffer (GetModuleFileNameW and
/// the like): called again with a bigger buffer until the whole path fits, so a
/// long folder name is never cut short.  fill(buffer, size) returns the length
/// it wrote, or `size` when the path did not fit, or 0 on failure.  Empty when
/// the call fails.
inline std::wstring wide_path_from(const std::function<DWORD(wchar_t*, DWORD)>& fill) {
    std::wstring w(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = fill(w.data(), static_cast<DWORD>(w.size()));
        if (n == 0) return {};
        if (n < w.size()) { w.resize(n); return w; }          // it fit, with room for the terminator
        if (w.size() >= kMaxWidePath) return {};
        w.resize(std::min(w.size() * 2, kMaxWidePath));
    }
}

/// A text value of the registry, or empty when the key or the value is missing,
/// is not text, or does not fit in MAX_PATH characters.  RegGetValueW with
/// RRF_RT_REG_SZ always ends the string with a terminator, so a value that was
/// stored without one cannot be read past the buffer (RegQueryValueExW does not
/// promise a terminator).
inline std::wstring registry_string(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[MAX_PATH] = {0};
    DWORD bytes = sizeof(buf);
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) return {};
    buf[MAX_PATH - 1] = L'\0';
    return buf;
}

}}  // namespace aoc::launcher

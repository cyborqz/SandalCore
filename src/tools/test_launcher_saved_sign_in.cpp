// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// test_launcher_saved_sign_in: "Keep me signed in" for launcher.exe
// (src/launcher_sign_in.h).
//
//   1. the saved sign-in in Windows Credential Manager: save / load / replace /
//      delete, under a TEST-ONLY target name (unique per run, deleted at the end;
//      the real name is never written, read or deleted; nothing is enumerated);
//   2. what a finished sign-in does to the saved one (by hand / automatic, the
//      box ticked or not, logout), run against the same test target;
//   3. how the auth server's answers are read;
//   4. which auth hosts the saved password may be sent to on its own (exactly
//      127.0.0.1, localhost, ::1 / [::1]; a table of spellings that are refused),
//      and where the auth server is (host and port of server.json, read once);
//   5. the automatic sign-in at start, with a fake clock and a fake auth server:
//      the "Signing in as ..." moment, Cancel, the server not up yet, a refusal;
//   6. LAUNCH GAME: the game client's command line (the one the launch script
//      used: "<exe>" -LauncherTetherPort=<port> <extra arguments>, -UseEOS=false
//      included, checked against the repo's own config\server.json) and which
//      running programs count as the game;
//   7. the sentences the window shows are plain words (no file names, ports,
//      tokens, scripts or "by itself" notes);
//   8. the request body (escapes, a round trip through a JSON parser) and that a
//      secret is wiped in place;
//   9. reading a reply: by length (a NUL is data), at most 64 KB, inside a
//      deadline, with a fake connection and a fake clock; and the deadline
//      watch that releases a call which cannot time itself;
//  10. reading this PC: a path of any length (a fake GetModuleFileNameW) and a
//      text value of the registry that was stored without a terminator (a
//      test-only key under HKEY_CURRENT_USER, deleted at the end);
//  11. the one-time move of what an earlier version saved under other names: a
//      saved sign-in between two test-only target names, and the remembered user
//      name file between two test-only folders (the real names and folders are
//      never touched).
//
// No password is printed: the passwords here are made-up strings, and the lines
// that name a check never hold one.
#include "launcher_sign_in.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace aoc::launcher;

static int g_passed = 0, g_failed = 0;

static void check(bool ok, const char* what) {
    if (ok) { std::printf("  ok   %s\n", what); ++g_passed; }
    else    { std::printf("  [FAIL] %s\n", what); ++g_failed; }
}

// ---------------------------------------------------------------------------
//  1 + 2: Credential Manager, under a test-only target
// ---------------------------------------------------------------------------

static std::wstring make_test_target() {
    return L"SandalCore/LauncherSignInTest/" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64());
}

// Deletes the test credential when the test ends, however it ends.
struct TargetGuard {
    std::wstring target;
    ~TargetGuard() { CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0); }
};

static bool same(const std::optional<SavedSignIn>& got, const wchar_t* user, const wchar_t* pass) {
    return got.has_value() && got->username == user && got->password == pass;
}

static void test_credential_round_trip(const wchar_t* T) {
    std::printf("-- saved sign-in in Credential Manager (test-only target)\n");

    check(!credential_load(T).has_value(), "nothing is saved under the test target at the start");
// (C | O | A ~ Cyb)

    const bool saved = credential_save(T, SavedSignIn{L"alice", L"first pass 1"});
    if (!saved) std::printf("       (Windows error %lu)\n", GetLastError());
    check(saved, "save a sign-in");
    check(same(credential_load(T), L"alice", L"first pass 1"), "load returns the user name and the password that were saved");

    // How Windows holds it: a generic credential kept for this user on this PC.
    {
        PCREDENTIALW c = nullptr;
        const bool read = CredReadW(T, CRED_TYPE_GENERIC, 0, &c) && c;
        check(read, "the credential is there as a generic credential");
        if (read) {
            check(c->Persist == CRED_PERSIST_LOCAL_MACHINE, "it persists for this Windows user on this PC (survives log-off and reboot)");
            check(c->TargetName && std::wstring(c->TargetName) == T, "it is stored under the target name that was given");
            if (c->CredentialBlob && c->CredentialBlobSize) SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
            CredFree(c);
        }
    }

    // A second save replaces the first (one entry, not two).
    check(credential_save(T, SavedSignIn{L"bob", L"p\u00e4ss w\u00f6rd \u20ac 2"}), "save again with another account");
    check(same(credential_load(T), L"bob", L"p\u00e4ss w\u00f6rd \u20ac 2"), "the second save replaced the first; a password with non-ASCII characters survives");

    // Refused saves leave what is there alone.
    check(!credential_save(T, SavedSignIn{L"", L"x"}), "an empty user name is not saved");
    check(!credential_save(T, SavedSignIn{L"carol", L""}), "an empty password is not saved");
    check(!credential_save(T, SavedSignIn{L"carol", std::wstring(2000, L'x')}), "a password too long for Credential Manager is not saved");
    check(!credential_save(nullptr, SavedSignIn{L"carol", L"x"}), "no target name, no save");
    check(same(credential_load(T), L"bob", L"p\u00e4ss w\u00f6rd \u20ac 2"), "after those refusals the saved sign-in is unchanged");

    // Delete.
    check(credential_remove(T), "delete the saved sign-in");
    check(!credential_load(T).has_value(), "after the delete nothing is saved");
    check(credential_remove(T), "deleting when nothing is saved counts as done");
    check(!credential_load(L"").has_value(), "load with an empty target name finds nothing");
    check(!credential_remove(nullptr), "delete with no target name is refused");

    // Someone edited the entry in Credential Manager: not a user name with a UTF-16 password.
    {
        BYTE odd[3] = {1, 2, 3};
        CREDENTIALW c{};
        c.Type = CRED_TYPE_GENERIC;
        c.TargetName = const_cast<LPWSTR>(T);
        c.UserName = const_cast<LPWSTR>(L"mallory");
        c.CredentialBlobSize = sizeof(odd);
        c.CredentialBlob = odd;
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        check(CredWriteW(&c, 0) == TRUE, "write an entry whose secret is not UTF-16 text");
        check(!credential_load(T).has_value(), "such an entry is not read as a sign-in");
        credential_remove(T);
    }
}

// The window's choices, run against the real Credential Manager calls (test target).
static void apply(const wchar_t* T, Remember what, const SavedSignIn& s) {
    switch (what) {
    case Remember::Save:   credential_save(T, s); break;
    case Remember::Delete: credential_remove(T); break;
    case Remember::Leave:  break;
    }
}

static void test_lifecycle(const wchar_t* T) {
    std::printf("-- what a sign-in does to the saved sign-in\n");
    const SavedSignIn alice{L"alice", L"alice pass"};
    const SavedSignIn bob{L"bob", L"bob pass"};

    check(remember_action(false, true, true) == Remember::Save, "by hand, it worked, box ticked: save");
    check(remember_action(false, true, false) == Remember::Delete, "by hand, it worked, box not ticked: forget");
    check(remember_action(false, false, true) == Remember::Leave, "by hand, it failed (typo), box ticked: leave it");
    check(remember_action(false, false, false) == Remember::Leave, "by hand, it failed, box not ticked: leave it");
    check(remember_action(true, true, true) == Remember::Leave, "automatic and it worked: it is saved already, leave it");
    check(remember_action(true, false, true) == Remember::Leave, "automatic and refused: keep the saved sign-in");
    check(remember_action(true, false, false) == Remember::Leave, "automatic and cancelled or no server: keep it");

    credential_remove(T);
    apply(T, remember_action(false, true, true), alice);
    check(same(credential_load(T), L"alice", L"alice pass"), "first sign-in by hand with the box ticked: saved");
    apply(T, remember_action(true, false, true), alice);
    check(same(credential_load(T), L"alice", L"alice pass"), "the automatic sign-in is refused: still saved");
    apply(T, remember_action(false, false, false), bob);
    check(same(credential_load(T), L"alice", L"alice pass"), "a failed attempt by hand (even with the box off) does not touch it");
    apply(T, remember_action(false, true, true), bob);
    check(same(credential_load(T), L"bob", L"bob pass"), "a sign-in by hand with the box ticked replaces it");
    apply(T, remember_action(false, true, false), alice);
    check(!credential_load(T).has_value(), "a sign-in by hand with the box not ticked deletes it");

    apply(T, remember_action(false, true, true), alice);
    check(credential_load(T).has_value(), "saved again");
    check(credential_remove(T) && !credential_load(T).has_value(), "Switch Account / Logout deletes it");
}

// ---------------------------------------------------------------------------
//  3: the auth server's answers
// ---------------------------------------------------------------------------

static void test_parse_reply() {
    std::printf("-- reading the auth server's answers\n");
    {
        const Reply r = parse_reply(R"({"access_token":"aoc_0123abcd","token_type":"Bearer","expires_in":86400,"username":"alice"})");
        check(r.kind == ReplyKind::Token && r.token == "aoc_0123abcd", "a reply with an access_token is a token");
    }
    {
        const Reply r = parse_reply("");
        check(r.kind == ReplyKind::NotReady && r.message == "Can't reach the sign-in server. Is the server running?", "no answer at all: not ready, and the sentence says the server may not be running");
    }
    {
        const Reply r = parse_reply(R"({"error":"server_error","message":"The login server failed; try again."})");
        check(r.kind == ReplyKind::NotReady && r.message == "The sign-in server had a problem. Please try again.", "server_error (its database is not up yet): not ready, in the window's own words");
    }
    {
        const Reply r = parse_reply(R"({"error":"server_error"})");
        check(r.kind == ReplyKind::NotReady && r.message == "The sign-in server had a problem. Please try again.", "server_error without a message gets the same sentence");
    }
    {
        const Reply r = parse_reply(R"({"error":"invalid_credentials","message":"Wrong username or password."})");
        check(r.kind == ReplyKind::Refused && r.message == "Wrong username or password.", "wrong password or unknown account: refused, with the server's sentence");
    }
    {
        const Reply r = parse_reply(R"({"error":"invalid_request","message":"Body must be JSON with username and password."})");
        check(r.kind == ReplyKind::Refused, "a bad request is refused, not retried");
    }
    {
        const Reply r = parse_reply(R"({"error":"username_taken"})");
        check(r.kind == ReplyKind::Refused && r.message == "Sign-in failed.", "no message: a plain sentence, never a bare error code");
    }
    {
        const Reply r = parse_reply(R"({"status":"x"})");
        check(r.kind == ReplyKind::Refused && r.message == "Sign-in failed.", "neither a token nor an error: refused with the plain sentence");
    }
    {
        const Reply r = parse_reply(R"({"access_token":""})");
        check(r.kind == ReplyKind::Refused && r.token.empty() && r.message == "The sign-in server sent an unexpected reply.", "an empty access_token is not a sign-in");
    }
    {
        const Reply r = parse_reply(R"({"access_token":12345})");
        check(r.kind == ReplyKind::Refused && r.token.empty(), "an access_token that is not a string is not a sign-in");
    }
    {
        const Reply r = parse_reply("<html>502 Bad Gateway</html>");
        check(r.kind == ReplyKind::Refused && r.message == "The sign-in server sent an unexpected reply.", "an answer that is not JSON is refused with a plain sentence");
    }
    {
        const Reply r = parse_reply("[1,2,3]");
        check(r.kind == ReplyKind::Refused, "JSON that is not an object is refused");
    }
    {
        const Reply r = parse_reply(R"({"error":5,"message":["x"]})");
        check(r.kind == ReplyKind::Refused && r.message == "Sign-in failed.", "fields of the wrong type do not throw");
    }
    {
        // A JSON parser stops reading at a NUL, so what follows would be ignored: a NUL is never part of an answer.
        std::string nul = R"({"access_token":"abc"})";
        nul += '\0';
        nul += "junk";
        const Reply r = parse_reply(nul);
        check(r.kind == ReplyKind::Refused && r.token.empty() && r.message == "The sign-in server sent an unexpected reply.",
              "an answer with a NUL inside is refused, not read up to the NUL");
    }
}
// [C-O-A ~ Cyb]

// ---------------------------------------------------------------------------
//  4: where the saved password may be sent on its own
// ---------------------------------------------------------------------------

static void test_is_local_host() {
    std::printf("-- the auth host the saved password may go to: exactly 127.0.0.1, localhost, ::1 or [::1]\n");
    check(is_local_host("127.0.0.1"), "127.0.0.1 is this PC");
    check(is_local_host("localhost") && is_local_host("LocalHost") && is_local_host("LOCALHOST") && is_local_host("lOcAlHoSt"),
          "localhost is this PC, in any case");
    check(is_local_host("::1") && is_local_host("[::1]"), "::1 and [::1] are this PC");

    // Every spelling below is refused: only the three names above are this PC.
    struct Refused { const char* why; std::string host; };
    const std::vector<Refused> refused = {
        {"no host at all", ""},
        {"blanks only", "   "},
        {"another 127.x.y.z address", "127.1.2.3"},
        {"127.0.0.2", "127.0.0.2"},
        {"the top of 127.x.y.z", "127.255.255.255"},
        {"a leading zero (0127.0.0.1)", "0127.0.0.1"},
        {"a zero in front of a part (127.0.0.01)", "127.0.0.01"},
        {"zeros padding every part", "127.000.000.001"},
        {"a dot after the address", "127.0.0.1."},
        {"a name that starts with the address", "127.0.0.1.example.com"},
        {"a name that starts with 127", "127.evil.com"},
        {"a name that starts with localhost", "localhost.example.com"},
        {"localhost with a dot after it", "localhost."},
        {"localhost with a letter added", "localhosts"},
        {"localhost with a letter in front", "xlocalhost"},
        {"a space inside localhost", "local host"},
        {"0.0.0.0", "0.0.0.0"},
        {"a port after the address", "127.0.0.1:8081"},
        {"a port after localhost", "localhost:8081"},
        {"a port after [::1]", "[::1]:8081"},
        {"blanks in front", " 127.0.0.1"},
        {"blanks after", "127.0.0.1 "},
        {"blanks around", "  127.0.0.1  "},
        {"blanks around localhost", " localhost "},
        {"a tab in front", "\t127.0.0.1"},
        {"a line break after", "127.0.0.1\r\n"},
        {"a NUL after the address", std::string("127.0.0.1\0", 10)},
        {"a NUL inside a name", std::string("localhost\0.evil.com", 19)},
        {"the short form 127.1", "127.1"},
        {"the number 2130706433", "2130706433"},
        {"the hex number 0x7f000001", "0x7f000001"},
        {"hex parts (0x7f.0.0.1)", "0x7f.0.0.1"},
        {"the octal number 017700000001", "017700000001"},
        {"a mapped IPv6 address", "::ffff:127.0.0.1"},
        {"::1 written out", "0:0:0:0:0:0:0:1"},
        {"[::1] written out", "[0:0:0:0:0:0:0:1]"},
        {"::0001", "::0001"},
        {"::1 with a zone", "::1%1"},
        {"an opening bracket only", "[::1"},
        {"a closing bracket only", "::1]"},
        {"double brackets", "[[::1]]"},
        {"the unspecified address ::", "::"},
        {"a user name in front", "user@127.0.0.1"},
        {"an address with a scheme", "http://127.0.0.1"},
        {"a slash after the address", "127.0.0.1/8"},
        {"a sign in front", "-127.0.0.1"},
        {"127.0.0.256, not an address", "127.0.0.256"},
        {"127.0.0, not a full address", "127.0.0"},
        {"an address on another network", "198.51.100.20"},
        {"another address on another network", "203.0.113.5"},
        {"a name on the internet", "example.com"},
    };
    for (const Refused& r : refused) {
        const std::string what = std::string("refused: ") + r.why;
        check(!is_local_host(r.host), what.c_str());
    }
}
// ~ C-Y-B & CoA ~

static void test_auth_endpoint() {
    std::printf("-- where the auth server is (host and port of server.json)\n");
    using nlohmann::json;
    {
        const AuthEndpoint e = auth_endpoint(json::parse(R"({"auth_server_host":"127.0.0.1","auth_server_port":8081})"));
        check(e.host == "127.0.0.1" && e.port == 8081, "host and port come from server.json");
    }
    {
        const AuthEndpoint e = auth_endpoint(json::parse(R"({"auth_server_host":"auth.example.com","auth_server_port":9000})"));
        check(e.host == "auth.example.com" && e.port == 9000, "another host and port are read as they are written");
    }
    {
        const AuthEndpoint e = auth_endpoint(json::parse("{}"));
        check(e.host == "127.0.0.1" && e.port == 8081, "no keys: 127.0.0.1 and port 8081");
    }
    {
        const AuthEndpoint e = auth_endpoint(json::parse(R"({"auth_server_host":5,"auth_server_port":"x"})"));
        check(e.host == "127.0.0.1" && e.port == 8081, "values of the wrong type keep the defaults");
    }
    check(auth_endpoint(json::parse("[1,2]")).port == 8081 && auth_endpoint(json::parse("null")).host == "127.0.0.1",
          "a server.json that is not an object gives the defaults");
    check(auth_endpoint(json::parse(R"({"auth_server_port":0})")).port == 8081 &&
          auth_endpoint(json::parse(R"({"auth_server_port":-1})")).port == 8081 &&
          auth_endpoint(json::parse(R"({"auth_server_port":65536})")).port == 8081 &&
          auth_endpoint(json::parse(R"({"auth_server_port":70000})")).port == 8081 &&
          auth_endpoint(json::parse(R"({"auth_server_port":8081.5})")).port == 8081,
          "a port outside 1-65535, or not a whole number, is ignored");
    check(auth_endpoint(json::parse(R"({"auth_server_port":1})")).port == 1 &&
          auth_endpoint(json::parse(R"({"auth_server_port":65535})")).port == 65535,
          "the ends of the range are ports");
    check(auth_endpoint(json::parse(R"({"auth_server_host":""})")).host.empty(), "an empty host stays empty (and is not this PC)");

    // The automatic sign-in: only this PC, and the answer is one value to keep.
    {
        const auto e = auto_sign_in_endpoint(json::parse(R"({"auth_server_host":"localhost","auth_server_port":9090})"));
        check(e && e->host == "localhost" && e->port == 9090, "a server on this PC: the sign-in gets its host and port");
    }
    {
        const auto e = auto_sign_in_endpoint(json::parse("{}"));
        check(e && e->host == "127.0.0.1" && e->port == 8081, "no keys: the defaults are this PC");
    }
    check(!auto_sign_in_endpoint(json::parse(R"({"auth_server_host":"203.0.113.5"})")) &&
          !auto_sign_in_endpoint(json::parse(R"({"auth_server_host":"127.0.0.1.example.com"})")) &&
          !auto_sign_in_endpoint(json::parse(R"({"auth_server_host":"0127.0.0.1"})")) &&
          !auto_sign_in_endpoint(json::parse(R"({"auth_server_host":"127.0.0.1:8081"})")) &&
          !auto_sign_in_endpoint(json::parse(R"({"auth_server_host":""})")),
          "a server that is not this PC: no automatic sign-in");
    {
        // The host that was checked is the host that is used, whatever the file says afterwards.
        json file = json::parse(R"({"auth_server_host":"127.0.0.1","auth_server_port":8081})");
        const auto kept = auto_sign_in_endpoint(file);
        file["auth_server_host"] = "attacker.example.com";
        file["auth_server_port"] = 4444;
        check(kept && kept->host == "127.0.0.1" && kept->port == 8081,
              "what was read at the start does not change when server.json changes later");
    }
}

// ---------------------------------------------------------------------------
//  5: the automatic sign-in, with a fake clock and a fake auth server
// ---------------------------------------------------------------------------

static Reply tok(const char* t) { Reply r; r.kind = ReplyKind::Token; r.token = t; return r; }
static Reply refused(const char* m) { Reply r; r.kind = ReplyKind::Refused; r.message = m; return r; }
static Reply not_ready() { return Reply{}; }

struct Sim {
    Ms clock{0};
    Ms cancel_at{-1};                  // Cancel is pressed once the clock reaches this (-1: never)
    bool cancel_in_request = false;    // Cancel is pressed while the request is out
    bool cancel_flag = false;
    std::vector<Reply> replies;        // one per request; the last one repeats
    bool store_ok = true;

    std::vector<Ms> request_at;
    std::vector<SavedSignIn> asked;
    std::vector<std::string> stored;
    std::vector<std::string> shown;

    AutoOps ops() {
        AutoOps o;
        o.cancelled = [this] { return cancel_flag || (cancel_at.count() >= 0 && clock >= cancel_at); };
        o.show = [this](const std::string& s) { shown.push_back(s); };
        o.sleep = [this](Ms d) { clock += d; };
        o.now = [this] { return clock; };
        o.request = [this](const SavedSignIn& s) {
            request_at.push_back(clock);
            asked.push_back(s);
            const Reply r = replies[std::min(request_at.size(), replies.size()) - 1];
            if (cancel_in_request) cancel_flag = true;
            return r;
        };
        o.store_token = [this](const std::string& t) { stored.push_back(t); return store_ok; };
        return o;
    }
    bool shown_has(const char* text) const {
        for (const auto& s : shown) if (s.find(text) != std::string::npos) return true;
        return false;
    }
};

static const SavedSignIn kAlice{L"alice", L"pw-alice-secret"};

static void test_auto_sign_in() {
    std::printf("-- the automatic sign-in at start\n");
// ~ Cyb - C | O | A ~

    {   // the server is up: one request, after the 3 s moment
        Sim s; s.replies = {tok("aoc_token_1")};
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::SignedIn && r.attempts == 1, "server up: signed in with one request");
        check(s.request_at.size() == 1 && s.request_at[0] == Ms{3000}, "the first request goes out after the 3 s 'Signing in as' moment, not before");
        check(s.asked.size() == 1 && s.asked[0].username == L"alice" && s.asked[0].password == L"pw-alice-secret", "it asks with the saved user name and password");
        check(s.stored.size() == 1 && s.stored[0] == "aoc_token_1", "the token file gets the token, once");
        check(s.shown_has("Starting in 3 s") && s.shown_has("Cancel"), "the window is told the 3 s countdown and that Cancel works");
        check(!s.shown_has("pw-alice-secret"), "the password never reaches the window text");
    }
    {   // Cancel during the 3 s
        Sim s; s.replies = {tok("aoc_token_2")}; s.cancel_at = Ms{1000};
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::Cancelled && r.attempts == 0, "Cancel in the first seconds: cancelled, no request made");
        check(s.stored.empty() && s.request_at.empty(), "nothing is asked and no token is written");
        check(s.clock >= Ms{1000} && s.clock <= Ms{1200}, "Cancel is noticed within a fraction of a second");
    }
    {   // Cancel before it starts
        Sim s; s.replies = {tok("aoc_token_3")}; s.cancel_flag = true;
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::Cancelled && s.clock == Ms{0} && s.request_at.empty(), "already cancelled: it does nothing at all");
    }
    {   // Cancel while the request is out: its token is dropped
        Sim s; s.replies = {tok("aoc_token_4")}; s.cancel_in_request = true;
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::Cancelled && r.attempts == 1, "Cancel while the request is out: cancelled");
        check(s.stored.empty(), "a token that comes back after Cancel is never written (the game must not start)");
    }
    {   // the server comes up after a while
        Sim s;
        s.replies = {parse_reply(""), parse_reply(R"({"error":"server_error","message":"The login server failed; try again."})"),
                     parse_reply(R"({"access_token":"aoc_token_5"})")};
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::SignedIn && r.attempts == 3, "no answer, then a server error, then the token: signed in on the third request");
        check(s.request_at.size() == 3 && s.request_at[1] - s.request_at[0] == Ms{2000} && s.request_at[2] - s.request_at[1] == Ms{2000},
              "it asks again every 2 s while the server is not up");
        check(s.stored.size() == 1 && s.stored[0] == "aoc_token_5", "one token file write, with the token");
        check(s.shown_has("isn't ready yet"), "the window says the sign-in server is not ready yet");
    }
    {   // the server never comes up
        Sim s; s.replies = {not_ready()};
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::GaveUp, "server never up: gives up");
        check(r.attempts >= 30 && r.attempts <= 32, "after about 30 requests, one every 2 s");
        check(s.clock >= Ms{63000} && s.clock <= Ms{66000}, "after about 60 s of asking (counted from the first request, after the 3 s moment)");
        check(r.message == "The sign-in server isn't answering.", "the message says the sign-in server is not answering");
        check(s.stored.empty(), "nothing is written");
    }
    {   // Cancel while waiting for the server
        Sim s; s.replies = {not_ready()}; s.cancel_at = Ms{10000};
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::Cancelled && r.attempts >= 3 && r.attempts <= 5, "Cancel while waiting for the server: stops within a fraction of a second");
        check(s.clock <= Ms{10300}, "it does not wait out the 2 s pause");
    }
    {   // refused: wrong password, unknown account, another database
        Sim s; s.replies = {parse_reply(R"({"error":"invalid_credentials","message":"Wrong username or password."})")};
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::Refused && r.message == "Wrong username or password.", "refused: the outcome and the server's sentence");
        check(r.attempts == 1 && s.request_at.size() == 1, "a refusal is not retried");
        check(s.stored.empty(), "nothing is written");
    }
    {   // refused after a not-ready
        Sim s; s.replies = {not_ready(), refused("Wrong username or password.")};
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::Refused && r.attempts == 2, "not ready, then refused: refused on the second request");
    }
    {   // the token file cannot be written
        Sim s; s.replies = {tok("aoc_token_6")}; s.store_ok = false;
        const AutoResult r = auto_sign_in(kAlice, s.ops());
        check(r.outcome == AutoOutcome::TokenNotSaved && r.message == "Signed in, but your sign-in could not be passed to the game. Please try again.",
              "the server accepts but the token file cannot be written: said plainly, not signed in");
    }
    {   // timing is a parameter
        Sim s; s.replies = {not_ready()};
        AutoTiming t; t.start_delay = Ms{0}; t.retry_every = Ms{10}; t.give_up_after = Ms{50}; t.slice = Ms{5};
        const AutoResult r = auto_sign_in(kAlice, s.ops(), t);
        check(r.outcome == AutoOutcome::GaveUp && s.request_at.size() >= 5 && s.request_at.size() <= 7 && s.request_at[0] == Ms{0},
              "other timings work the same way (no start delay, 10 ms retries, 50 ms limit)");
    }
}

// ---------------------------------------------------------------------------
//  6: LAUNCH GAME: the game client's command line, and the check for a client that runs
// ---------------------------------------------------------------------------

static const char kSteamExe[] =
    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Ashes of Creation\\Game\\AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe";
static const char kSteamDir[] =
    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Ashes of Creation\\Game\\AOC\\Binaries\\Win64\\";

// This tree's config\server.json: the repo's (dist\Release is two folders below it), else one next to the exe.
static std::string find_server_json() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const std::filesystem::path dir = std::filesystem::path(buf).parent_path();
    for (const wchar_t* rel : {L"..\\..\\config\\server.json", L"config\\server.json"}) {
        std::error_code ec;
        const std::filesystem::path p = dir / rel;
        if (std::filesystem::exists(p, ec) && !ec) return p.string();
    }
    return {};
}

static void test_start_game() {
    std::printf("-- LAUNCH GAME: starting the game client\n");
    using nlohmann::json;

    // config\server.json -> the tether port and the extra arguments
    {
        const LaunchSettings s = launch_settings(json::parse(R"({"tether_port":19021,"client_extra_args":"-UseEOS=false","auth_server_port":8081})"));
        check(s.tether_port == 19021 && s.client_args == "-UseEOS=false", "the tether port and the extra arguments come from server.json");
    }
    {
        const LaunchSettings s = launch_settings(json::parse("{}"));
        check(s.tether_port == 19021 && s.client_args.empty(), "no keys in server.json: port 19021, no extra arguments");
    }
    {
        const LaunchSettings s = launch_settings(json::parse(R"({"tether_port":"x","client_extra_args":5})"));
        check(s.tether_port == 19021 && s.client_args.empty(), "values of the wrong type are ignored");
    }
    {
        check(launch_settings(json::parse(R"({"tether_port":0})")).tether_port == 19021 &&
              launch_settings(json::parse(R"({"tether_port":70000})")).tether_port == 19021 &&
              launch_settings(json::parse(R"({"tether_port":-5})")).tether_port == 19021,
              "a tether port outside 1-65535 is ignored");
        check(launch_settings(json::parse(R"({"tether_port":7000})")).tether_port == 7000, "another tether port is used");
    }
    check(launch_settings(json::parse("[1,2]")).tether_port == 19021 && launch_settings(json::parse("null")).client_args.empty(),
          "a server.json that is not an object gives the defaults");
    check(launch_settings(json::parse(R"({"client_extra_args":"  -UseEOS=false \r\n"})")).client_args == "-UseEOS=false",
          "blanks around the extra arguments are dropped");

    // the command line: what the launch script ran, "<game exe>" -LauncherTetherPort=19021 -UseEOS=false
    {
        LaunchSettings s; s.tether_port = 19021; s.client_args = "-UseEOS=false";
        const ClientCommand c = client_command(kSteamExe, s);
        check(c.command_line == std::string("\"") + kSteamExe + "\" -LauncherTetherPort=19021 -UseEOS=false",
              "the command line is the old script's: the quoted exe, the tether port, -UseEOS=false");
        check(c.command_line.find("-UseEOS=false") != std::string::npos, "-UseEOS=false arrives");
        check(c.working_dir == kSteamDir, "the client starts in its own folder (<game root>\\AOC\\Binaries\\Win64)");
    }
    {
        LaunchSettings s; s.tether_port = 7000;
        check(client_command(kSteamExe, s).command_line == std::string("\"") + kSteamExe + "\" -LauncherTetherPort=7000",
              "no extra arguments: no trailing blank, no -UseEOS");
        s.client_args = "-UseEOS=false -windowed";
        check(client_command(kSteamExe, s).command_line == std::string("\"") + kSteamExe + "\" -LauncherTetherPort=7000 -UseEOS=false -windowed",
              "several extra arguments are passed as they are");
    }
    {
        const ClientCommand c = client_command("D:/Games/AOC/Binaries/Win64/AOCClient-Win64-Shipping.exe", LaunchSettings{});
        check(c.working_dir == "D:/Games/AOC/Binaries/Win64/", "a path with forward slashes also gives its folder");
        check(client_command("AOCClient.exe", LaunchSettings{}).working_dir.empty(), "an exe without a folder: no working folder");
    }
// == CoA | C:Y:B ==

    // this tree's real config\server.json must keep starting the client without Epic Online Services
    {
        const std::string path = find_server_json();
        if (path.empty()) {
            std::printf("  [skip] config\\server.json not found next to this test or in the repo\n");
        } else {
            std::ifstream f(path);
            nlohmann::json j;
            try { j = nlohmann::json::parse(f); } catch (...) { j = nullptr; }
            const LaunchSettings s = launch_settings(j);
            check(s.tether_port == 19021, "the repo's server.json gives the tether port 19021 the game client is started with");
            check(client_command(kSteamExe, s).command_line.find("-UseEOS=false") != std::string::npos,
                  "the repo's server.json starts the client with -UseEOS=false");
        }
    }

    // which running programs count as the game
    check(is_game_process_name(L"AOCClient-Win64-Shipping.exe"), "AOCClient-Win64-Shipping.exe is the game");
    check(is_game_process_name(L"AOCClient.exe"), "AOCClient.exe is the game");
    check(is_game_process_name(L"aocclient-win64-shipping.EXE"), "the case does not matter");
    check(!is_game_process_name(L"AOCClientCrashReportClient.exe"), "the crash reporter is not the game");
    check(!is_game_process_name(L"launcher.exe") && !is_game_process_name(L"worldd.exe") && !is_game_process_name(L"notAOCClient.exe"),
          "other programs are not the game");
    check(!is_game_process_name(L"") && !is_game_process_name(L"AOCClient-.exe") && !is_game_process_name(L"AOCClient-Win64-Shipping.txt"),
          "nothing, a bare dash and another extension are not the game");
}

// ---------------------------------------------------------------------------
//  7: the sentences the window shows are plain words
// ---------------------------------------------------------------------------

// True when s holds no file name, port, token, script or server name.
static bool plain_words(const std::string& s) {
    std::string t = s;
    for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* bad : {"token", ".txt", ".json", ".exe", "auth", "port", "script", "itself", "http", "127.", "localhost", "session",
                            "executable", "config", "jwt", "grpc"})
        if (t.find(bad) != std::string::npos) return false;
    return true;
}

static void test_player_wording() {
    std::printf("-- the window's sentences are plain words\n");
    check(plain_words("Wrong username or password.") && plain_words("Sign in again, please."), "the check passes plain sentences");
    check(!plain_words("Session token active in data\\ics_auth_token.txt") && !plain_words("Ready to connect on port 7777.") &&
          !plain_words("The game starts by itself.") && !plain_words("Chosen by the launch script."),
          "and it catches the developer lines the window used to show");

    bool all = true;
    for (const char* m : {kMsgCannotReach, kMsgUnexpected, kMsgServerTrouble, kMsgSignInFailed, kMsgNotPassedOn, kMsgNotAnswering})
        all = all && plain_words(m);
    check(all, "the six sentences for a sign-in that does not work");
    check(plain_words(parse_reply("").message) && plain_words(parse_reply("<html>").message) && plain_words(parse_reply(R"({"error":"server_error"})").message) &&
          plain_words(parse_reply(R"({"access_token":""})").message) && plain_words(parse_reply(R"({"error":"x"})").message),
          "every sentence parse_reply gives for a bad answer");

    // everything the automatic sign-in tells the window: the countdown, the attempts, the waiting, the give-up
    Sim s; s.replies = {not_ready(), not_ready(), tok("aoc_token_w")};
    const AutoResult ok = auto_sign_in(kAlice, s.ops());
    bool lines = !s.shown.empty();
    for (const auto& l : s.shown) lines = lines && plain_words(l);
    check(ok.outcome == AutoOutcome::SignedIn && lines, "the countdown, 'contacting' and 'not ready yet' lines of the automatic sign-in");
    Sim g; g.replies = {not_ready()};
    const AutoResult gave = auto_sign_in(kAlice, g.ops());
    bool glines = true;
    for (const auto& l : g.shown) glines = glines && plain_words(l);
    check(gave.outcome == AutoOutcome::GaveUp && glines && plain_words(gave.message), "and when it gives up");
    Sim f; f.replies = {tok("aoc_token_f")}; f.store_ok = false;
    check(plain_words(auto_sign_in(kAlice, f.ops()).message), "and when the sign-in cannot be passed to the game");
}

// ---------------------------------------------------------------------------
//  8: the request body, and wiping a secret
// ---------------------------------------------------------------------------

static void test_login_body() {
    std::printf("-- the request body, and wiping it\n");
    using nlohmann::json;

    check(login_body("alice", "pw") == R"({"username":"alice","password":"pw"})", "the body holds the user name and the password");

    const std::string tricky = std::string("q\"uo\\te\n\t\x01 end \xc3\xa4\xe2\x82\xac") + '/' + '\x1f' + '\x7f';
    const std::string body = login_body("bo\"b", tricky);
    json j;
    try { j = json::parse(body); } catch (...) { j = nullptr; }
    check(j.is_object() && j.size() == 2 && j.value("username", "") == "bo\"b" && j.value("password", "") == tricky,
          "quotes, backslashes, control characters and UTF-8 come back unchanged through a JSON parser");
    bool raw_control = false;
    for (const char c : body) raw_control = raw_control || static_cast<unsigned char>(c) < 0x20;
    check(!raw_control, "no raw control character is left in the body");
    check(body.find('\0') == std::string::npos, "no NUL in the body");

    // A long password does not move around while the body is built.
    const std::string longpass(120, 'p');
    check(json::parse(login_body("u", longpass)).value("password", "") == longpass, "a 120 character password is written whole");

    // wipe(): the bytes are zeroed in place (the buffer stays allocated, so it can be looked at), then the string is empty.
    {
        std::string s = login_body("alice", "hunter2-secret-hunter2");
        const char* p = s.data();
        const std::size_t n = s.size();
        check(n > 40, "the body to wipe is long enough to sit in the heap");
        wipe(s);
        bool zero = true;
        for (std::size_t i = 0; i < n; ++i) zero = zero && p[i] == '\0';
        check(s.empty() && zero, "wipe zeroes every byte in place and empties the string");
    }
    {
        std::wstring w = L"a long wide password to be wiped in place";
        const wchar_t* p = w.data();
        const std::size_t n = w.size();
        wipe(w);
        bool zero = true;
        for (std::size_t i = 0; i < n; ++i) zero = zero && p[i] == L'\0';
        check(w.empty() && zero, "wipe zeroes a wide string in place too");
    }
    {
        std::string e;
        wipe(e);
        check(e.empty(), "wiping an empty string is fine");
    }
}

// ---------------------------------------------------------------------------
//  9: reading a reply: by length, at most 64 KB, inside a deadline
// ---------------------------------------------------------------------------
// C.Y.B with C-O-A

// A fake connection with a fake clock: hands out `data` in pieces, and every read costs `cost` of the clock.
struct FakeConn {
    std::string data;
    std::size_t piece = 4096;
    std::size_t pos = 0;
    Ms clock{0};
    Ms cost{0};
    bool error_at_end = false;
    std::size_t reads = 0;
    std::size_t biggest_ask = 0;

    long read(char* buf, std::size_t n, Ms) {
        ++reads;
        clock += cost;
        biggest_ask = std::max(biggest_ask, n);
        if (pos >= data.size()) return error_at_end ? -1 : 0;
        const std::size_t k = std::min({n, piece, data.size() - pos});
        std::memcpy(buf, data.data() + pos, k);
        pos += k;
        return static_cast<long>(k);
    }
};

static ReadResult read_from(FakeConn& c, Ms deadline_at, std::size_t max_bytes = kMaxReplyBytes) {
    return read_reply([&c](char* b, std::size_t n, Ms left) { return c.read(b, n, left); },
                      [&c] { return c.clock; }, deadline_at, max_bytes);
}

static void test_read_reply() {
    std::printf("-- reading a reply: by length, within a size and a deadline\n");
    const std::string token_json = R"({"access_token":"aoc_abc","token_type":"Bearer"})";

    {
        FakeConn c; c.data = token_json; c.piece = 7;
        const ReadResult r = read_from(c, Ms{15000});
        check(r.end == ReadEnd::Complete && r.body == token_json, "a reply that arrives in small pieces is put together");
        check(reply_from_read(r).kind == ReplyKind::Token && reply_from_read(r).token == "aoc_abc", "and it is read as the token it holds");
    }
    {
        FakeConn c;
        const ReadResult r = read_from(c, Ms{15000});
        check(r.end == ReadEnd::Complete && r.body.empty(), "a server that hangs up without a word: an empty, complete reply");
        const Reply x = reply_from_read(r);
        check(x.kind == ReplyKind::NotReady && x.message == "Can't reach the sign-in server. Is the server running?", "which reads as 'not ready' (asked again later)");
    }
    {   // bytes by length: a NUL inside the reply is data, not the end of it
        std::string with_nul = token_json;
        with_nul += '\0';
        with_nul += "tail after the NUL";
        FakeConn c; c.data = with_nul; c.piece = 9;
        const ReadResult r = read_from(c, Ms{15000});
        check(r.end == ReadEnd::Complete && r.body.size() == with_nul.size() && r.body == with_nul, "a NUL inside the reply does not cut it: every byte is kept");
        check(reply_from_read(r).kind == ReplyKind::Refused && reply_from_read(r).token.empty(),
              "and a reply with a NUL in it is refused, not read up to the NUL");
    }
    {   // the size limit: exactly the limit is a reply, one byte more is not
        FakeConn c; c.data = std::string(kMaxReplyBytes, 'a');
        const ReadResult r = read_from(c, Ms{15000});
        check(r.end == ReadEnd::Complete && r.body.size() == kMaxReplyBytes, "a reply of exactly 64 KB is read");
    }
    {
        FakeConn c; c.data = std::string(kMaxReplyBytes + 1, 'a');
        const ReadResult r = read_from(c, Ms{15000});
        check(r.end == ReadEnd::TooBig && r.body.empty(), "a reply of 64 KB and one byte is too big, and nothing of it is kept");
        const Reply x = reply_from_read(r);
        check(x.kind == ReplyKind::Refused && x.message == "The sign-in server sent an unexpected reply.", "which is refused as an unexpected reply");
    }
    {   // an endless reply is not read to its end
        FakeConn c; c.data = std::string(8 * 1024 * 1024, 'x');
        const ReadResult r = read_from(c, Ms{15000});
        check(r.end == ReadEnd::TooBig && c.pos <= kMaxReplyBytes + 4096, "an 8 MB reply is stopped after about 64 KB, not read to its end");
        check(c.biggest_ask <= 4096, "no read asks for more than 4 KB at a time");
    }
    {   // the deadline
        FakeConn c; c.data = std::string(100000, 'a'); c.piece = 10; c.cost = Ms{1000};
        const ReadResult r = read_from(c, Ms{5000});
        check(r.end == ReadEnd::TimedOut && r.body.empty(), "a server that dribbles bytes is stopped at the deadline");
        check(c.reads == 5 && c.clock == Ms{5000}, "after five 1-second reads of a 5-second deadline, no read starts later");
        const Reply x = reply_from_read(r);
        check(x.kind == ReplyKind::NotReady, "a reply that ran out of time reads as 'not ready'");
    }
    {   // already late: nothing is read at all
        FakeConn c; c.data = token_json;
        const ReadResult r = read_from(c, Ms{0});
        check(r.end == ReadEnd::TimedOut && c.reads == 0, "a deadline that has passed reads nothing");
    }
    {   // each read is told how long it may wait: what is left of the deadline
        std::vector<long long> told;
        std::string data = token_json;
        std::size_t pos = 0;
        Ms clock{0};
        const ReadResult r = read_reply(
            [&](char* b, std::size_t n, Ms left) -> long {
                told.push_back(left.count());
                clock += Ms{400};
                const std::size_t k = std::min({n, std::size_t{10}, data.size() - pos});
                std::memcpy(b, data.data() + pos, k);
                pos += k;
                return static_cast<long>(k);
            },
            [&] { return clock; }, Ms{3000});
        check(r.end == ReadEnd::Complete && told.size() >= 3 && told[0] == 3000 && told[1] == 2600 && told[2] == 2200,
              "each read is told how long it may wait: what is left of the deadline");
    }
    {   // a broken connection
        FakeConn c; c.data = "{\"acc"; c.error_at_end = true;
        const ReadResult r = read_from(c, Ms{15000});
        check(r.end == ReadEnd::Failed && r.body.empty(), "a connection that breaks half way: failed, nothing kept");
        check(reply_from_read(r).kind == ReplyKind::NotReady, "which reads as 'not ready'");
    }
    {   // what a redirect means
        ReadResult r; r.end = ReadEnd::Redirected;
        const Reply x = reply_from_read(r);
        check(x.kind == ReplyKind::Refused && x.message == "The sign-in server sent an unexpected reply.", "a redirect is refused as an unexpected reply (it is never followed)");
    }
    {
        ReadResult r; r.end = ReadEnd::Complete; r.body = R"({"error":"invalid_credentials","message":"Wrong username or password."})";
        const Reply x = reply_from_read(r);
        check(x.kind == ReplyKind::Refused && x.message == "Wrong username or password.", "a complete refusal is read as the server's sentence");
    }
    {
        ReadResult r;   // never read: Failed
        check(reply_from_read(r).kind == ReplyKind::NotReady, "no reply at all reads as 'not ready'");
    }
}
// ~ C_Y_B & CoA ~

// The deadline watch keeps the time for a call that cannot be given a deadline of its own (a WinInet read that
// waits until its buffer is full): when the time is up it runs `cancel`, which makes that call fail.
static void test_deadline_watch() {
    std::printf("-- the deadline watch\n");
    using clk = std::chrono::steady_clock;
    {   // the time runs out while a call is blocked: the cancel releases it
        std::mutex m;
        std::condition_variable cv;
        bool cancelled = false;
        const auto t0 = clk::now();
        DeadlineWatch watch(
            [&] { { std::lock_guard<std::mutex> l(m); cancelled = true; } cv.notify_all(); },
            t0 + std::chrono::milliseconds(150));
        {   // the blocked call: it would wait 20 s
            std::unique_lock<std::mutex> l(m);
            cv.wait_for(l, std::chrono::seconds(20), [&] { return cancelled; });
        }
        const Ms waited = std::chrono::duration_cast<Ms>(clk::now() - t0);
        check(watch.stop() && cancelled, "the time is up: the cancel runs, and stop() says the deadline came first");
        check(waited >= Ms{140} && waited < Ms{5000}, "a call that would block for 20 s is released at the deadline");
    }
    {   // the call ends in time: nothing is cancelled, and stop() does not wait for the deadline
        std::atomic<int> cancels{0};
        const auto t0 = clk::now();
        DeadlineWatch watch([&] { ++cancels; }, t0 + std::chrono::seconds(30));
        const bool fired = watch.stop();
        check(!fired && cancels == 0, "the call ends before the deadline: nothing is cancelled, stop() says so");
        check(std::chrono::duration_cast<Ms>(clk::now() - t0) < Ms{3000}, "stop() returns at once, it does not wait out the deadline");
        check(!watch.stop(), "stop() can be called again");
    }
    {   // a watch that goes out of scope without stop() ends cleanly and never cancels early
        std::atomic<int> cancels{0};
        { DeadlineWatch watch([&] { ++cancels; }, clk::now() + std::chrono::seconds(30)); }
        check(cancels == 0, "a watch that goes out of scope before its deadline cancels nothing");
    }
    {   // a deadline that has passed cancels at once, and only once
        std::atomic<int> cancels{0};
        DeadlineWatch watch([&] { ++cancels; }, clk::now() - std::chrono::seconds(1));
        std::this_thread::sleep_for(Ms{300});
        check(cancels == 1 && watch.stop() && cancels == 1, "a deadline that has passed cancels once");
    }
}

// ---------------------------------------------------------------------------
//  10: reading this PC: a path of any length, a text value of the registry
// ---------------------------------------------------------------------------

// A fake GetModuleFileNameW for a module whose path is `path`: the path and its terminator when they fit; else the
// start of it, ended by a terminator, and `size` as the answer.
static DWORD fake_module_name(const std::wstring& path, wchar_t* buf, DWORD size) {
    if (size == 0) return 0;
    if (path.size() < size) {
        std::wmemcpy(buf, path.c_str(), path.size() + 1);
        return static_cast<DWORD>(path.size());
    }
    std::wmemcpy(buf, path.c_str(), size - 1);
    buf[size - 1] = L'\0';
    return size;
}

static void test_wide_path_from() {
    std::printf("-- the launcher's own folder, for a path of any length\n");
    for (const std::size_t len : {std::size_t{40}, std::size_t{259}, std::size_t{260}, std::size_t{261}, std::size_t{519},
                                  std::size_t{520}, std::size_t{1000}, std::size_t{5000}, std::size_t{32767}}) {
        std::wstring path = L"C:\\";
        while (path.size() < len) path += L"d\\";
        path.resize(len);
        int calls = 0;
        const std::wstring got = wide_path_from([&](wchar_t* b, DWORD n) { ++calls; return fake_module_name(path, b, n); });
        char what[96];
        std::snprintf(what, sizeof(what), "a path of %zu characters comes back whole (%d call%s)", len, calls, calls == 1 ? "" : "s");
        check(got == path, what);
    }
    {
        int calls = 0;
        const std::wstring got = wide_path_from([&](wchar_t*, DWORD) { ++calls; return DWORD{0}; });
        check(got.empty() && calls == 1, "a call that fails gives nothing, after one call");
    }
    {
        int calls = 0;
        const std::wstring got = wide_path_from([&](wchar_t*, DWORD n) { ++calls; return n; });   // never fits
        check(got.empty() && calls > 1 && calls < 20, "a call that never fits stops at the longest possible path");
    }
}

// Removes the test-only registry key when the test ends, however it ends.
struct RegKeyGuard {
    std::wstring subkey;
    ~RegKeyGuard() { RegDeleteKeyW(HKEY_CURRENT_USER, subkey.c_str()); }
};

static bool set_reg(HKEY k, const wchar_t* name, DWORD type, const void* data, std::size_t bytes) {
    return RegSetValueExW(k, name, 0, type, static_cast<const BYTE*>(data), static_cast<DWORD>(bytes)) == ERROR_SUCCESS;
}

static void test_registry_string() {
    std::printf("-- a text value of the registry (test-only key under HKEY_CURRENT_USER)\n");
    const std::wstring sub = L"Software\\SandalCoreLauncherTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                             std::to_wstring(GetTickCount64());
    check(registry_string(HKEY_CURRENT_USER, sub.c_str(), L"v").empty(), "a key that is not there gives nothing");

    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        std::printf("  [skip] the test-only registry key could not be created\n");
        return;
    }
    RegKeyGuard guard{sub};

    const wchar_t good[] = L"C:\\Games\\Steam";
    check(set_reg(k, L"good", REG_SZ, good, sizeof(good)), "store a normal text value");
    check(registry_string(HKEY_CURRENT_USER, sub.c_str(), L"good") == good, "a normal text value is read");
    check(registry_string(HKEY_CURRENT_USER, sub.c_str(), L"missing").empty(), "a value that is not there gives nothing");

    // Stored WITHOUT a terminator (RegQueryValueExW hands that back as it is).
    const wchar_t bare[] = L"abcde";
    check(set_reg(k, L"bare", REG_SZ, bare, 5 * sizeof(wchar_t)), "store a text value without its terminator");
    check(registry_string(HKEY_CURRENT_USER, sub.c_str(), L"bare") == L"abcde", "a value stored without a terminator is read, ended properly");

    // Stored without a terminator and exactly as long as the whole buffer: it must not be read past the end.
    {
        const std::wstring full(MAX_PATH, L'x');
        check(set_reg(k, L"full", REG_SZ, full.data(), full.size() * sizeof(wchar_t)), "store MAX_PATH characters with no terminator");
        check(registry_string(HKEY_CURRENT_USER, sub.c_str(), L"full").empty(), "which does not fit with its terminator: nothing is returned, nothing is read past the buffer");
    }
    {
        const std::wstring fits(MAX_PATH - 1, L'y');
        check(set_reg(k, L"fits", REG_SZ, fits.data(), fits.size() * sizeof(wchar_t)), "store MAX_PATH - 1 characters with no terminator");
        check(registry_string(HKEY_CURRENT_USER, sub.c_str(), L"fits") == fits, "MAX_PATH - 1 characters fit with their terminator and are read whole");
    }
    {
        const std::wstring longer(5000, L'z');
        check(set_reg(k, L"longer", REG_SZ, longer.c_str(), (longer.size() + 1) * sizeof(wchar_t)), "store a text value of 5000 characters");
        check(registry_string(HKEY_CURRENT_USER, sub.c_str(), L"longer").empty(), "a value longer than the buffer gives nothing");
    }
    {
        const DWORD number = 7;
        check(set_reg(k, L"number", REG_DWORD, &number, sizeof(number)), "store a number");
        check(registry_string(HKEY_CURRENT_USER, sub.c_str(), L"number").empty(), "a number is not text");
        const BYTE bin[4] = {'a', 0, 'b', 0};
        check(set_reg(k, L"bin", REG_BINARY, bin, sizeof(bin)), "store binary data");
        check(registry_string(HKEY_CURRENT_USER, sub.c_str(), L"bin").empty(), "binary data is not text");
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
//  11: the one-time move of what an earlier version saved under other names
// ---------------------------------------------------------------------------

// Two test-only target names ("earlier" and "current"), unique per run, both deleted when the test ends.  The real
// names are never read, written or deleted.
struct TwoTargets {
    std::wstring earlier, current;
    ~TwoTargets() {
        CredDeleteW(earlier.c_str(), CRED_TYPE_GENERIC, 0);
        CredDeleteW(current.c_str(), CRED_TYPE_GENERIC, 0);
    }
};
// C-Y-B + C | O | A

static std::wstring unique_suffix() {
    return std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
}

static void test_credential_move() {
    std::printf("-- moving a saved sign-in to another target name (two test-only targets)\n");
    TwoTargets t{L"SandalCore/LauncherSignInTest/Earlier-" + unique_suffix(), L"SandalCore/LauncherSignInTest/Current-" + unique_suffix()};
    const wchar_t* E = t.earlier.c_str();
    const wchar_t* C = t.current.c_str();
    const SavedSignIn alice{L"alice", L"päss wörd € 1"};
    const SavedSignIn bob{L"bob", L"bob pass 2"};

    check(!credential_exists(E) && !credential_exists(C), "neither test target holds anything at the start");
    check(credential_move(E, C) == Moved::Nothing && !credential_exists(E) && !credential_exists(C), "nothing under the earlier name: nothing is moved, nothing is created");

    // The case the move is for: a sign-in under the earlier name, nothing under the current one.
    check(credential_save(E, alice), "save a sign-in under the earlier name");
    check(credential_move(E, C) == Moved::Done, "it is moved");
    check(same(credential_load(C), L"alice", L"päss wörd € 1"), "the current name holds the same user name and password (non-ASCII survives)");
    check(!credential_exists(E), "the earlier entry is deleted");
    {
        PCREDENTIALW c = nullptr;
        const bool read = CredReadW(C, CRED_TYPE_GENERIC, 0, &c) && c;
        check(read && c->Persist == CRED_PERSIST_LOCAL_MACHINE && c->TargetName && std::wstring(c->TargetName) == C,
              "the moved entry is a normal saved sign-in: kept for this Windows user, under the current name");
        if (read) { if (c->CredentialBlob && c->CredentialBlobSize) SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize); CredFree(c); }
    }
    check(credential_move(E, C) == Moved::Nothing && same(credential_load(C), L"alice", L"päss wörd € 1"), "a second start moves nothing and changes nothing");

    // Both names hold a sign-in: the current one wins, the earlier one is left alone.
    check(credential_save(E, bob), "save another sign-in under the earlier name");
    check(credential_move(E, C) == Moved::Nothing, "the current name is taken: nothing is moved");
    check(same(credential_load(C), L"alice", L"päss wörd € 1") && same(credential_load(E), L"bob", L"bob pass 2"), "both entries are unchanged");
    check(credential_remove(E), "delete the earlier entry");

    // Only the current name holds one.
    check(credential_move(E, C) == Moved::Nothing && same(credential_load(C), L"alice", L"päss wörd € 1"), "only the current name holds a sign-in: unchanged");
    check(credential_remove(C), "delete the current entry");

    // An earlier entry that is not a sign-in (someone edited it in Credential Manager) is left alone.
    {
        BYTE odd[3] = {1, 2, 3};
        CREDENTIALW c{};
        c.Type = CRED_TYPE_GENERIC;
        c.TargetName = const_cast<LPWSTR>(E);
        c.UserName = const_cast<LPWSTR>(L"mallory");
        c.CredentialBlobSize = sizeof(odd);
        c.CredentialBlob = odd;
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        check(CredWriteW(&c, 0) == TRUE, "write an earlier entry whose secret is not UTF-16 text");
        check(credential_move(E, C) == Moved::Nothing && credential_exists(E) && !credential_exists(C), "it is not moved, not deleted, and nothing is created");
        check(credential_remove(E), "delete it");
    }

    // The same name, however it is written, is never moved onto itself (that would delete it).
    check(credential_save(E, alice), "save a sign-in for the same-name checks");
    std::wstring shouting = t.earlier;
    for (wchar_t& ch : shouting) if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - (L'a' - L'A'));
    check(credential_move(E, E) == Moved::Nothing && same(credential_load(E), L"alice", L"päss wörd € 1"), "the same name: nothing is moved, nothing is deleted");
    check(credential_move(E, shouting.c_str()) == Moved::Nothing && same(credential_load(E), L"alice", L"päss wörd € 1"),
          "the same name in capital letters is the same entry: nothing is moved, nothing is deleted");

    // Windows refuses the new entry: the earlier one stays.
    {
        const std::wstring too_long(CRED_MAX_GENERIC_TARGET_NAME_LENGTH + 100, L'x');
        check(credential_move(E, too_long.c_str()) == Moved::Failed && same(credential_load(E), L"alice", L"päss wörd € 1"),
              "Windows refuses the new entry: reported as failed, the earlier sign-in is still there");
    }

    // Names that are not names.
    check(credential_move(nullptr, C) == Moved::Nothing && credential_move(E, nullptr) == Moved::Nothing &&
          credential_move(L"", C) == Moved::Nothing && credential_move(E, L"") == Moved::Nothing, "a missing or empty name moves nothing");
    check(same(credential_load(E), L"alice", L"päss wörd € 1") && !credential_exists(C), "and leaves everything as it was");

    check(!credential_exists(nullptr) && !credential_exists(L""), "no name, no entry");
    check(credential_remove(E), "delete the test entry");
    check(!credential_exists(E) && !credential_exists(C), "neither test target holds anything at the end");
}

// Removes the test-only folder (and what is in it) when the test ends.
struct TempDir {
    std::filesystem::path dir;
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};

static std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool spit(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return static_cast<bool>(f);
}

static void test_copy_if_missing() {
    namespace fs = std::filesystem;
    std::printf("-- copying the remembered user name file to its new place (test-only folder)\n");
    TempDir t{fs::temp_directory_path() / (L"SandalCoreLauncherTest-" + unique_suffix())};
    const fs::path earlier = t.dir / L"earlier" / L"launcher.json";
    const fs::path current = t.dir / L"current" / L"deeper" / L"launcher.json";
    const std::string text = "{\n  \"username\": \"alice\"\n}\n";

    check(!copy_if_missing(earlier, current) && !fs::exists(current), "no file at the earlier place: nothing is copied");
    check(spit(earlier, text), "write the earlier file");
    check(copy_if_missing(earlier, current), "the earlier file is copied to a place that does not exist yet");
    check(slurp(current) == text && fs::exists(earlier) && slurp(earlier) == text, "the copy is the same, and the earlier file stays");
    check(!fs::exists(fs::path(current).concat(L".tmp")), "no temporary file is left behind");
// ~ C_Y_B :: C O A ~

    check(spit(current, "{\"username\":\"bob\"}"), "put another file at the new place");
    check(!copy_if_missing(earlier, current) && slurp(current) == "{\"username\":\"bob\"}", "a file that is there already is never overwritten");

    check(!copy_if_missing(fs::path{}, current) && !copy_if_missing(earlier, fs::path{}), "empty paths do nothing");
    check(!copy_if_missing(t.dir / L"earlier", t.dir / L"somewhere" / L"launcher.json") && !fs::exists(t.dir / L"somewhere"),
          "a folder is not a file: nothing is copied");
    bool threw = false, copied = false;
    try { copied = copy_if_missing(earlier, t.dir / L"bad<>|?*" / L"launcher.json"); } catch (...) { threw = true; }
    check(!threw && !copied, "a path Windows cannot make is refused without throwing");
}

int main() {
    std::printf("=== launcher: Keep me signed in, LAUNCH GAME ===\n");

    // The test-only target: unique per run, never the real name.
    TargetGuard guard{make_test_target()};
    const wchar_t* T = guard.target.c_str();
    check(std::wstring(kCredentialTarget) == L"SandalCore/LauncherSignIn", "the target name every launcher shares is SandalCore/LauncherSignIn");
    check(guard.target != kCredentialTarget && guard.target.rfind(L"SandalCore/LauncherSignInTest/", 0) == 0,
          "this test uses its own target name, never the real one");

    test_credential_round_trip(T);
    test_lifecycle(T);
    test_parse_reply();
    test_is_local_host();
    test_auth_endpoint();
    test_auto_sign_in();
    test_start_game();
    test_player_wording();
    test_login_body();
    test_read_reply();
    test_deadline_watch();
    test_wide_path_from();
    test_registry_string();
    test_credential_move();
    test_copy_if_missing();

    check(!credential_load(T).has_value(), "the test target holds nothing at the end");

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

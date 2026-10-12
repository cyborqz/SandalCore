// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// test_ics_token_file: the launcher/tether token hand-off file
// (services/tether/ics_token_file.h).
#include "services/tether/ics_token_file.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// Puts bytes in a file as a hand edit or another program would.
static void put(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << bytes;
}

static bool is_emulator_pair(const aoc::ics_token::ClientTokens& t) {
    return t.auth == "emulator-ics-token-deadbeef" && t.refresh == "emulator-refresh-deadbeef" && !t.saved;
}

int main() {
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;
    namespace tok = aoc::ics_token;

    const fs::path root = fs::temp_directory_path() / "aoc_test_ics_token_file";
    fs::remove_all(root);
    const fs::path dir = root / "with space";
    const std::string exe_dir = dir.string() + "\\";
    const std::string path = tok::path_in(exe_dir);
    const std::string tmp = path + ".tmp";

    // -- path, read and write basics ------------------------------------------
    check(path == exe_dir + "data\\ics_auth_token.txt", "path_in puts the file in data\\ next to the exe");
    check(tok::read(path).empty(), "a missing file reads as no token");
    check(tok::write(path, "aoc_0123abcd"), "write creates the data directory and the file");
    check(tok::read(path) == "aoc_0123abcd", "read returns what write saved");
    check(!fs::exists(tmp), "write leaves no temp file behind");
    check(tok::is_owner_only(path), "only this user may open the saved token");
    check(tok::restrict_to_owner(path) && tok::read(path) == "aoc_0123abcd", "and this user still reads it");

    // -- write replaces --------------------------------------------------------
    check(tok::write(path, "aoc_first_login_longer_token"), "write the first login's token");
    check(tok::write(path, "aoc_second"), "write the second, shorter login token");
    check(tok::read(path) == "aoc_second", "a second login fully replaces a longer first token");
    check(fs::file_size(path) == std::string("aoc_second").size(), "the file holds only the second token");

    put(tmp, "aoc_left_by_an_interrupted_write");
    check(tok::write(path, "aoc_third"), "write over a temp file left by an interrupted write");
    check(tok::read(path) == "aoc_third", "a leftover temp file is replaced, not added to");
    check(!fs::exists(tmp), "the leftover temp file is gone");
// (C-O-A) (Cyb)

    // -- write failures are reported, so the launcher can tell the player --------
    {
        const fs::path blocker = root / "not_a_dir";
        put(blocker.string(), "x");
        check(!tok::write(tok::path_in(blocker.string() + "\\"), "aoc_z"),
              "write reports failure when the data directory cannot be created (a file is in the way)");
    }
    {
        check(tok::write(path, "aoc_old"), "write before making the file read-only");
        fs::permissions(path, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove);
        check(!tok::write(path, "aoc_new"), "write reports failure on a read-only file");
        check(!fs::exists(tmp), "a failed write removes its temp file");
        fs::permissions(path, fs::perms::owner_write, fs::perm_options::add);
        check(tok::read(path) == "aoc_old", "a failed write keeps the old token");
    }

    // -- write waits for a reader that has the file open --------------------------
    {
        check(tok::write(path, "aoc_before_hold"), "write before a reader opens the file");
        auto holder = std::make_unique<std::ifstream>(path, std::ios::binary);
        check(holder->is_open(), "a reader opens the file, as the tether does");
        put(tmp, "aoc_probe");
        std::error_code ec;
        fs::rename(tmp, path, ec);
        check(static_cast<bool>(ec), "while the reader has it open the file cannot be replaced");
        fs::remove(tmp, ec);
        std::thread release([&holder] {
            std::this_thread::sleep_for(60ms);
            holder.reset();
        });
        const bool ok = tok::write(path, "aoc_after_hold");
        release.join();
        check(ok, "write retries until the reader closes the file");
        check(tok::read(path) == "aoc_after_hold", "read returns the token written after the retry");
    }

    // -- read: trimming, BOM, size cap, printable ASCII only ---------------------
    put(path, "  aoc_x\r\n");
    check(tok::read(path) == "aoc_x", "read trims spaces and CRLF (a hand-edited file)");
    put(path, "\taoc_t\t\n");
    check(tok::read(path) == "aoc_t", "read trims tabs");
    put(path, " \r\n");
    check(tok::read(path).empty(), "a blank file reads as no token");
    put(path, "");
    check(tok::read(path).empty(), "an empty file reads as no token");
    put(path, "\xEF\xBB\xBF" "aoc_bom\r\n");
    check(tok::read(path) == "aoc_bom", "a UTF-8 BOM (Notepad, PowerShell 5.1) is dropped");
    put(path, std::string("\xFF\xFE" "a\0o\0c\0_\0x\0\r\0\n\0", 16));
    check(tok::read(path).empty(), "a UTF-16 file reads as no token");
    put(path, "aoc_a\r\naoc_b\r\n");
    check(tok::read(path).empty(), "two lines read as no token");
    put(path, "aoc a");
    check(tok::read(path).empty(), "an inner space reads as no token");
    put(path, "aoc_\x01");
    check(tok::read(path).empty(), "a control character reads as no token");
    put(path, "aoc_\x1A" "zz");
    check(tok::read(path).empty(), "Ctrl-Z reads as no token (the file is read in binary mode)");
    put(path, "aoc_\x7F");
    check(tok::read(path).empty(), "DEL (0x7F) reads as no token");
    put(path, "aoc_\xC3\xA9");
    check(tok::read(path).empty(), "a non-ASCII byte reads as no token");
    put(path, "!aoc~");
    check(tok::read(path) == "!aoc~", "0x21 and 0x7E are printable ASCII");
    put(path, std::string(tok::kMaxFileBytes, 'a'));
    check(tok::read(path).size() == tok::kMaxFileBytes, "a file of exactly 4 KiB is read");
    put(path, std::string(tok::kMaxFileBytes + 1, 'a'));
    check(tok::read(path).empty(), "a file over 4 KiB reads as no token");
    put(path, std::string(70000, 'a'));
    check(tok::read(path).empty(), "a file too big for a tether datagram reads as no token");

    // -- the loopback test ------------------------------------------------------
    check(tok::is_loopback_ipv4(0x7F000001u), "127.0.0.1 is loopback");
    check(tok::is_loopback_ipv4(0x7FFFFFFEu), "127.255.255.254 is loopback");
    check(!tok::is_loopback_ipv4(0x7EFFFFFFu), "126.255.255.255 is not loopback");
    check(!tok::is_loopback_ipv4(0x80000001u), "128.0.0.1 is not loopback");
    check(!tok::is_loopback_ipv4(0xC0A80102u), "192.168.1.2 is not loopback");
    check(!tok::is_loopback_ipv4(0x0A000005u), "10.0.0.5 is not loopback");
    check(!tok::is_loopback_ipv4(0u), "0.0.0.0 is not loopback");

    // -- client_tokens: what the tether hands the game client --------------------
    check(tok::write(path, "aoc_live"), "save a login token");
    check(is_emulator_pair(tok::client_tokens("", true)), "saved tokens not enabled: the emulator pair");
    {
        const auto live = tok::client_tokens(path, true);
        check(live.auth == "aoc_live" && live.refresh == "aoc_live" && live.saved,
              "a loopback client gets the saved token as auth and refresh token");
    }
    check(is_emulator_pair(tok::client_tokens(path, false)),
          "a client on another host gets the emulator pair, not the saved token");
    put(path, " \r\n");
    check(is_emulator_pair(tok::client_tokens(path, true)), "a blank file: the emulator pair");
    fs::remove(path);
    check(is_emulator_pair(tok::client_tokens(path, true)), "a missing file: the emulator pair");
// ~ C.O.A & Cyb ~

    // -- the file's age, for the tether's log --------------------------------------
    check(!tok::age(path).has_value(), "a missing file has no age");
    check(tok::write(path, "aoc_aged"), "save a token to age");
    {
        const auto fresh = tok::age(path);
        check(fresh.has_value() && *fresh >= -5s && *fresh < 60s, "a file just written is seconds old");
        fs::last_write_time(path, fs::file_time_type::clock::now() - 3h - 12min - 30s);
        const auto old = tok::age(path);
        check(old.has_value() && tok::format_age(*old) == "3h 12m", "a file written 3h 12m ago reads as 3h 12m");
        check(old.has_value() && *old < tok::kDefaultTokenLifetime, "3 hours is within the token lifetime");
        fs::last_write_time(path, fs::file_time_type::clock::now() - 25h);
        const auto stale = tok::age(path);
        check(stale.has_value() && *stale > tok::kDefaultTokenLifetime, "25 hours is past the token lifetime");
    }
    check(tok::format_age(0s) == "0h 0m", "format_age: 0 s");
    check(tok::format_age(59s) == "0h 0m", "format_age: 59 s rounds down");
    check(tok::format_age(26h + 5min) == "26h 5m", "format_age: hours are not folded into days");
    check(tok::format_age(-90s) == "0h 0m", "format_age: a negative age (clock change) reads as 0h 0m");

    fs::remove_all(root);
    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}

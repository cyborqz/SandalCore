// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ===========================================================================
//  launcher_main.cpp  –  SandalCore Emulator Launcher (Modern UI)
//
//  Win32 GUI. No gRPC/protobuf — WinInet + nlohmann_json only.
//
//  The only things it talks to or touches:
//    config\server.json         read   auth_server_host / auth_server_port (the
//                                      local auth server, plain HTTP), tether_port,
//                                      client_extra_args
//    config\paths.json          read, and write next to the exe: game_executable
//    data\ics_auth_token.txt    write  the session token after a sign-in; the
//                                      tether hands it to the game client
//                                      (services/tether/ics_token_file.h)
//    %APPDATA%\SandalCore\launcher.json
//                               read + write: the username, never the password
//    Windows Credential Manager one entry, "SandalCore/LauncherSignIn":
//                               read + write + delete: "Keep me signed in"
//                               (below, src/launcher_sign_in.h)
//    Steam's registry key and steamapps\libraryfolders.vdf   read (Auto-Detect)
//    the Windows file dialog (Browse)
//    the list of running programs, names only: LAUNCH GAME looks for a game
//                               client that already runs
//    the game client: LAUNCH GAME starts it, and only a press of that button does
//
//  Keep me signed in:
//    The sign-in form has a "Keep me signed in" box.  A sign-in by hand that
//    works with the box ticked saves the user name and password in Windows
//    Credential Manager (for this Windows user on this PC; no file, no log).
//    At the next start, in any copy of the launcher, a saved sign-in shows
//    "Signing in as <name>..." with a Cancel button for 3 seconds, then signs in
//    against this run's auth server (asking again for up to 60 seconds while it
//    is not up yet) and writes the token file exactly as a sign-in by hand does.
//    It only does that for an auth server on this PC: the host in
//    config\server.json must be exactly 127.0.0.1, localhost or ::1, and the
//    host and port read at the start are used for every retry.  A refusal (wrong
//    password, unknown account, another database) keeps the saved sign-in and
//    shows the form with the username filled in; a sign-in by hand with the box
//    ticked replaces it, with the box not ticked deletes it, and Switch Account
//    / Logout deletes it.  It never presses LAUNCH GAME or skips the game's own
//    screens: the player presses it.
//
//  LAUNCH GAME (enabled once signed in and the game is found) starts the game
//  client the way the launch script used to: "<game exe>" -LauncherTetherPort=
//  <tether_port> <client_extra_args> (config\server.json), in the client's own
//  folder.  While that client runs the button reads GAME RUNNING and is off; it
//  is on again when the client exits.  With a game client already running on the
//  PC it starts nothing and says so.
//
//  Command line:
//    (none)                     sign in, then press LAUNCH GAME
//    --screenshot <file>        draw the window into <file> (BMP data) and exit;
//                               no network, no file writes
//    --preview-state <name>     with --screenshot: draw that state instead of
//                               the signed-out form (signed-in, signing-in,
//                               form-kept: the box ticked, game-running)
//
//  Keyboard: the window is not a dialog, so the message loop in WinMain runs
//  every message through IsDialogMessage.  Tab / Shift+Tab walk the controls;
//  the focused owner-drawn button gets an outline (on_draw_item).  Enter presses
//  the focused button, else the default one: Login on the form, LAUNCH GAME once
//  signed in (DM_GETDEFID -> default_button_id).  The focus starts in the
//  password box when a username was remembered, else in the username box.
// ===========================================================================
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <wininet.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

#include <nlohmann/json.hpp>
#include "launcher_sign_in.h"
#include "services/tether/ics_token_file.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

using json = nlohmann::json;

// ===========================================================================
//  Modern Dark Gaming Color Palette
// ===========================================================================
static const COLORREF kBg        = RGB( 18,  22,  33); // #121621 Window bg
static const COLORREF kCardBg    = RGB( 18,  22,  33); // #121621 Box bg (seamless, no black edges)
static const COLORREF kCardBdr   = RGB( 34,  44,  65); // #222c41 Divider / border
static const COLORREF kFieldBg   = RGB( 12,  15,  23); // #0c0f17 Input surface
static const COLORREF kFieldBdr  = RGB( 38,  48,  70); // #263046 Input border
static const COLORREF kAccent    = RGB(225,  45,  45); // #e12d2d Ashes Crimson
static const COLORREF kAccentHv  = RGB(245,  65,  65); // #f54141 Bright hover
static const COLORREF kAccentPs  = RGB(185,  30,  30); // #b91e1e Deep pressed
static const COLORREF kAccentDs  = RGB( 42,  30,  34); // Disabled accent
static const COLORREF kBtnDark   = RGB( 28,  36,  53); // #1c2435 Secondary button
static const COLORREF kBtnDkHv   = RGB( 40,  50,  74); // #28324a Hover
static const COLORREF kBtnDkPs   = RGB( 22,  28,  42); // Pressed
static const COLORREF kTxtWht    = RGB(248, 250, 254); // Clean white
static const COLORREF kTxtSub    = RGB(145, 155, 178); // Secondary text
static const COLORREF kTxtDim    = RGB( 88,  98, 120); // Muted text
static const COLORREF kTxtOk     = RGB( 52, 199,  89); // Green
static const COLORREF kTxtErr    = RGB(255,  69,  58); // Red
static const COLORREF kTxtWarn   = RGB(255, 179,  64); // Amber
// (C.O.A) (Cyb)

// ===========================================================================
//  Window & Box Layout Dimensions (Only Login, Client Path, Launch Game)
// ===========================================================================
constexpr int kW = 450;

constexpr int kCardX   = 0;
constexpr int kCardW   = kW;
constexpr int kCard1Y  = 0;
constexpr int kCard1H  = 220; // Box 1: Account Login (the form, the session, "Signing in as ...")
constexpr int kCard2Y  = 220;
constexpr int kCard2H  = 82;  // Box 2: Client path
constexpr int kCard3Y  = 302;
constexpr int kCard3H  = 96;  // Box 3: Launch Game

constexpr int kH       = 398;

// Box 1, top to bottom
constexpr int kKeepY      = 136;  // "Keep me signed in" (form)
constexpr int kAuthBtnY   = 164;  // Login / Register (form)
constexpr int kFormFbY    = 200;  // feedback line, form
constexpr int kSessLineY  = 56;   // "Signed in as ..." and "Signing in as ..."
constexpr int kSessNoteY  = 84;   // the line under it
constexpr int kSessBtnY   = 120;  // Switch Account / Logout, Cancel
constexpr int kSessFbY    = 166;  // feedback line, session and "Signing in as ..."

// ===========================================================================
//  Control IDs
// ===========================================================================
enum {
    ID_USER       = 101,
    ID_PASS       = 102,
    ID_LOGIN      = 103,
    ID_REG        = 104,
    ID_LAUNCH     = 105,
    ID_BROWSE     = 106,
    ID_AUTODETECT = 107,
    ID_LOGOUT     = 108,
    ID_KEEP       = 109,   // "Keep me signed in"
    ID_CANCEL     = IDCANCEL,   // Cancel the automatic sign-in (Esc presses it, as in a dialog)
};

// ===========================================================================
//  GDI Resources
// ===========================================================================
static HBRUSH g_brBg      = nullptr;
static HBRUSH g_brCard    = nullptr;
static HBRUSH g_brField   = nullptr;
static HFONT  g_fntHeader = nullptr;
static HFONT  g_fntMed    = nullptr;
static HFONT  g_fntBig    = nullptr;
static HFONT  g_fntForm   = nullptr;
static HFONT  g_fntLabel  = nullptr;
static HFONT  g_fntSmall  = nullptr;

// ===========================================================================
//  Application State
// ===========================================================================
static bool         g_loggedIn    = false;
static std::wstring g_username;
static std::wstring g_feedback;
static bool         g_feedbackErr = false;
static std::string  g_gameExe;
static bool         g_gameExeOk   = false;   // g_gameExe exists; checked when it changes, not at every paint

// Command line (parse_command_line)
static bool         g_screenshotMode   = false;
static std::wstring g_previewState;

// The game client this window started and that still runs (LAUNCH GAME reads GAME RUNNING)
static bool         g_gameRunning      = false;
// C O A with Cyb

// Hover flags
static bool g_hoverLogin      = false;
static bool g_hoverReg        = false;
static bool g_hoverLaunch     = false;
static bool g_hoverBrowse     = false;
static bool g_hoverAutoDetect = false;
static bool g_hoverLogout     = false;
static bool g_hoverKeep       = false;
static bool g_hoverCancel     = false;

// Keep me signed in
static bool         g_keepSignedIn = false;  // the box on the form; ticked while a sign-in is saved
static bool         g_autoActive   = false;  // "Signing in as <g_autoUser>..." is showing
static std::wstring g_autoUser;
static std::wstring g_autoStatus;            // the small line under it
static std::atomic<bool> g_autoCancel{false};

static std::atomic<bool> g_authBusy{false};
constexpr UINT WM_AUTH_DONE   = WM_APP + 1;
constexpr UINT WM_AUTO_STATUS = WM_APP + 2;
constexpr UINT WM_AUTO_DONE   = WM_APP + 3;
constexpr UINT WM_GAME_EXITED = WM_APP + 4;   // the game client this window started has ended
// message is the sentence shown to the player; username is what was typed; note
// is an extra sentence for a sign-in that worked (the sign-in could not be saved).
struct AuthResult { bool ok; std::string message; std::string username; std::string note; };
// How the automatic sign-in ended (launcher_sign_in.h).
struct AutoDone { aoc::launcher::AutoOutcome outcome; std::string message; std::wstring username; };

static HWND g_hwnd = nullptr;
static HWND g_lastFocus = nullptr;

// ===========================================================================
//  String & Path Utilities
// ===========================================================================
static std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

static std::string to_utf8(const wchar_t* w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// The folder of launcher.exe with its closing backslash, in the narrow form the
// file functions below take.  The wide API, with a buffer that grows until the
// whole path fits: a long install folder is never cut short.
static std::string exe_dir()
{
    std::wstring w = aoc::launcher::wide_path_from(
        [](wchar_t* buf, DWORD size) { return ::GetModuleFileNameW(nullptr, buf, size); });
    const auto p = w.find_last_of(L"\\/");
    if (p == std::wstring::npos) return "";
    w.resize(p + 1);
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string json_str(const std::string& path, const std::string& key,
                            const std::string& def = "")
{
    try {
        std::ifstream f(path); if (!f) return def;
        return json::parse(f).value(key, def);
    } catch (...) { return def; }
}

// True if the file exists.  The error_code overload: an odd path (a dead
// network share, a bad character) reads as "not there" instead of throwing.
static bool file_exists(const std::string& p)
{
    if (p.empty()) return false;
    std::error_code ec;
    const bool e = std::filesystem::exists(std::filesystem::path(p), ec);
    return e && !ec;
}

// ===========================================================================
//  Game Path (config\paths.json, key game_executable)
// ===========================================================================
static std::vector<std::string> config_paths_candidates()
{
    std::vector<std::string> paths;
    const std::string dir = exe_dir();
    paths.push_back(dir + "config\\paths.json");
    paths.push_back(dir + "..\\..\\config\\paths.json");
    paths.push_back(dir + "..\\config\\paths.json");
    return paths;
}
// (C O A ~ C | Y | B)

static std::string load_game_exe()
{
    for (const auto& p : config_paths_candidates()) {
        if (file_exists(p)) {
            std::string exe = json_str(p, "game_executable", "");
            if (file_exists(exe)) return exe;
        }
    }
    for (const auto& p : config_paths_candidates()) {
        if (file_exists(p)) {
            std::string exe = json_str(p, "game_executable", "");
            if (!exe.empty()) return exe;
        }
    }
    return json_str(exe_dir() + "config\\paths.json", "game_executable", "");
}

// Saves game_executable into config\paths.json next to the exe, and only
// there, and only when it differs from what the file holds.  The other
// candidates above are read as a fallback and never written: two folders up
// from dist\Release is the repo's own tracked config\paths.json.
static bool save_game_exe(const std::string& exe_path)
{
    const std::string p = exe_dir() + "config\\paths.json";
    try {
        json j = json::object();
        {
            std::ifstream in(p);
            if (in) {
                try { j = json::parse(in); } catch (...) { j = json::object(); }
            }
        }
        if (!j.is_object()) j = json::object();
        if (j.contains("game_executable") && j["game_executable"].is_string() &&
            j["game_executable"].get<std::string>() == exe_path)
            return true;
        j["game_executable"] = exe_path;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
        std::ofstream out(p, std::ios::trunc);
        if (!out) return false;
        out << j.dump(4, ' ', false, json::error_handler_t::replace) << "\n";
        out.flush();
        return static_cast<bool>(out);
    } catch (...) { return false; }
}

static void set_game_exe(const std::string& path)
{
    g_gameExe = path;
    g_gameExeOk = file_exists(path);
}

// ===========================================================================
//  Auto-Detect Steam & Ashes of Creation Executable
// ===========================================================================
static std::string auto_detect_game()
{
    std::vector<std::string> searchRoots;

    // aoc::launcher::registry_string reads with RegGetValueW and RRF_RT_REG_SZ: a text
    // value, always ended by a terminator, never read past its buffer.
    auto queryRegString = [](HKEY root, const wchar_t* subkey, const wchar_t* valName) -> std::string {
        return to_utf8(aoc::launcher::registry_string(root, subkey, valName).c_str());
    };

    std::string steamPath = queryRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steamPath.empty()) {
        steamPath = queryRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    }
    if (steamPath.empty()) {
        steamPath = queryRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    }

    if (!steamPath.empty()) {
        for (char& c : steamPath) if (c == '/') c = '\\';
        searchRoots.push_back(steamPath);

        std::string vdfPath = steamPath + "\\steamapps\\libraryfolders.vdf";
        std::ifstream vdf(vdfPath);
        if (vdf) {
            std::string line;
            while (std::getline(vdf, line)) {
                size_t p = line.find("\"path\"");
                if (p != std::string::npos) {
                    size_t q1 = line.find('\"', p + 6);
                    if (q1 != std::string::npos) {
                        size_t q2 = line.find('\"', q1 + 1);
                        if (q2 != std::string::npos) {
                            std::string lib = line.substr(q1 + 1, q2 - q1 - 1);
                            std::string cleanLib;
                            for (size_t i = 0; i < lib.size(); ++i) {
                                if (lib[i] == '\\' && i + 1 < lib.size() && lib[i + 1] == '\\') {
                                    cleanLib += '\\';
                                    ++i;
                                } else {
                                    cleanLib += lib[i];
                                }
                            }
                            if (!cleanLib.empty()) searchRoots.push_back(cleanLib);
                        }
                    }
                }
            }
        }
    }

    // The usual install folders on every fixed drive.  Network, removable and
    // optical drives are skipped: probing an offline share can stall the window.
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (drives & (1u << i)) {
            char dLetter = 'A' + i;
            std::string d = std::string(1, dLetter) + ":";
            if (GetDriveTypeA((d + "\\").c_str()) != DRIVE_FIXED) continue;
            searchRoots.push_back(d);
            searchRoots.push_back(d + "\\Program Files (x86)\\Steam");
            searchRoots.push_back(d + "\\Steam");
            searchRoots.push_back(d + "\\Games");
            searchRoots.push_back(d + "\\Ashes of Creation");
            searchRoots.push_back(d + "\\AshesOfCreation");
        }
    }

    std::vector<std::string> uniqueRoots;
    for (const auto& r : searchRoots) {
        if (!r.empty() && std::find(uniqueRoots.begin(), uniqueRoots.end(), r) == uniqueRoots.end()) {
            uniqueRoots.push_back(r);
        }
    }

    const std::vector<std::string> subPaths = {
        "\\steamapps\\common\\Ashes of Creation\\Game\\AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe",
        "\\steamapps\\common\\Ashes of Creation\\Game\\AOC\\Binaries\\Win64\\AOCClient.exe",
        "\\Game\\AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe",
        "\\Game\\AOC\\Binaries\\Win64\\AOCClient.exe",
        "\\AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe",
        "\\AOC\\Binaries\\Win64\\AOCClient.exe"
    };

    for (const auto& root : uniqueRoots) {
        for (const auto& sub : subPaths) {
            std::string full = root + sub;
            if (file_exists(full)) {
                if (full.size() >= 13 && full.ends_with("AOCClient.exe")) {
                    std::string ship = full.substr(0, full.size() - 13) + "AOCClient-Win64-Shipping.exe";
                    if (file_exists(ship)) return ship;
                }
                return full;
            }
        }
    }
// -- Cyb x C-O-A --

    return "";
}

// ===========================================================================
//  File Browser Dialog
// ===========================================================================
static std::string browse_for_game(HWND hwnd, const std::string& current_path)
{
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Ashes Executable (AOCClient*.exe)\0AOCClient*.exe;AOCClient-Win64-Shipping.exe\0All Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Select Ashes of Creation Executable";

    std::wstring initDir;
    if (!current_path.empty()) {
        try {
            std::filesystem::path p(current_path);
            if (file_exists(p.parent_path().string())) {
                initDir = p.parent_path().wstring();
                ofn.lpstrInitialDir = initDir.c_str();
            }
        } catch (...) {}
    }

    if (GetOpenFileNameW(&ofn)) {
        std::string selected = to_utf8(szFile);
        try {
            std::filesystem::path sp(selected);
            if (sp.filename() == "AOCClient.exe") {
                auto ship = sp.parent_path() / "AOCClient-Win64-Shipping.exe";
                if (file_exists(ship.string())) {
                    selected = ship.string();
                }
            }
        } catch (...) {}
        return selected;
    }
    return "";
}

// ===========================================================================
//  HTTP POST (WinInet)
// ===========================================================================
// The auth server is on this PC and answers in milliseconds.  One request gets
// kHttpDeadlineMs in all (connecting, sending and reading its reply together),
// each wait inside it at most kHttpWaitMs, and a reply of at most
// aoc::launcher::kMaxReplyBytes.  A redirect is never followed.
// Plain HTTP on purpose: the saved password only goes to this PC (loopback), so it never crosses a network.
constexpr DWORD kHttpDeadlineMs = 15000;
constexpr DWORD kHttpWaitMs     = 10000;

// The waits above only time the silence between two bytes, and WinInet keeps
// reading until its buffer is full: a server that sends a byte every few seconds
// could hold a request for hours.  So the deadline is kept by an
// aoc::launcher::DeadlineWatch that, when the time is up, closes the request from
// another thread; that makes the call blocked on it fail.
static aoc::launcher::ReadResult http_post(const aoc::launcher::AuthEndpoint& ep,
                                           const std::string& path, const std::string& body)
{
    namespace sign_in = aoc::launcher;
    const auto deadline_tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHttpDeadlineMs);
    const auto steady_now = [] {
        return std::chrono::duration_cast<sign_in::Ms>(std::chrono::steady_clock::now().time_since_epoch());
    };
    const sign_in::Ms deadline_at = std::chrono::duration_cast<sign_in::Ms>(deadline_tp.time_since_epoch());

    sign_in::ReadResult out;   // Failed until a reply has been read
    HINTERNET hI = InternetOpenA("AoCEmu/3.0", INTERNET_OPEN_TYPE_DIRECT,
                                 nullptr, nullptr, 0);
    if (!hI) return out;
    DWORD wait = kHttpWaitMs;
    InternetSetOptionA(hI, INTERNET_OPTION_CONNECT_TIMEOUT, &wait, sizeof(wait));
    InternetSetOptionA(hI, INTERNET_OPTION_SEND_TIMEOUT,    &wait, sizeof(wait));
    InternetSetOptionA(hI, INTERNET_OPTION_RECEIVE_TIMEOUT, &wait, sizeof(wait));
    HINTERNET hC = InternetConnectA(hI, ep.host.c_str(), (INTERNET_PORT)ep.port,
                                    nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hC) { InternetCloseHandle(hI); return out; }
    HINTERNET hR = HttpOpenRequestA(hC, "POST", path.c_str(), nullptr, nullptr,
                                    nullptr, INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTO_REDIRECT, 0);
    if (!hR) { InternetCloseHandle(hC); InternetCloseHandle(hI); return out; }

    // Without the watch (no thread to be had) the waits above still apply.
    std::unique_ptr<sign_in::DeadlineWatch> watch;
    try {
        watch = std::make_unique<sign_in::DeadlineWatch>([hR] { InternetCloseHandle(hR); }, deadline_tp);
    } catch (...) {}

    const char* hdrs = "Content-Type: application/json\r\n";
    if (HttpSendRequestA(hR, hdrs, (DWORD)strlen(hdrs),
                         const_cast<char*>(body.data()), (DWORD)body.size())) {
        DWORD status = 0, size = sizeof(status);
        if (HttpQueryInfoA(hR, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr) &&
            status >= 300 && status < 400) {
            out.end = sign_in::ReadEnd::Redirected;
        } else {
            // The bytes by length (a NUL inside is data), within the size and the
            // time that are left; each wait is cut to what is left of the deadline.
            const auto read_some = [hR](char* buf, std::size_t n, sign_in::Ms left) -> long {
                DWORD t = (DWORD)std::min<long long>(left.count(), kHttpWaitMs);
                InternetSetOptionA(hR, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
                DWORD got = 0;
                if (!InternetReadFile(hR, buf, (DWORD)n, &got)) return -1;
                return (long)got;
            };
            out = sign_in::read_reply(read_some, steady_now, deadline_at);
        }
    }

    bool request_open = true;
    if (watch && watch->stop()) {                       // the deadline came first
        request_open = false;                           // the watch closed the request, once
        if (out.end != sign_in::ReadEnd::Complete) { out.end = sign_in::ReadEnd::TimedOut; out.body.clear(); }
    }
    if (request_open) InternetCloseHandle(hR);
    InternetCloseHandle(hC); InternetCloseHandle(hI);
    return out;
}

// ===========================================================================
//  Game Launcher Process Execution
// ===========================================================================
// config\server.json as the JSON object it holds ({} when it is missing or broken).
static json load_server_json()
{
    try {
        std::ifstream f(exe_dir() + "config\\server.json");
        if (!f) return json::object();
        json j = json::parse(f);
        return j.is_object() ? j : json::object();
    } catch (...) { return json::object(); }
}

// LAUNCH GAME: starts the game client exactly as the launch script used to
// ("<game exe>" -LauncherTetherPort=<tether_port> <client_extra_args>, in the
// client's own folder; aoc::launcher::client_command).  Returns "" and the
// client's process handle (the caller closes it), or the sentence for the player.
static std::string start_game(HANDLE* process)
{
    *process = nullptr;
    if (g_gameExe.empty() || !file_exists(g_gameExe))
        return "The game was not found. Use Auto-Detect or Browse to select it.";

    const aoc::launcher::ClientCommand cmd =
        aoc::launcher::client_command(g_gameExe, aoc::launcher::launch_settings(load_server_json()));
    std::string line = cmd.command_line;   // CreateProcessA may write into it
// C.Y.B with CoA

    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessA(g_gameExe.c_str(), line.data(), nullptr, nullptr,
                          FALSE, 0, nullptr,
                          cmd.working_dir.empty() ? nullptr : cmd.working_dir.c_str(), &si, &pi)) {
        return "The game could not be started (error " + std::to_string(::GetLastError()) + ").";
    }

    ::CloseHandle(pi.hThread);
    *process = pi.hProcess;
    return "";
}

// Waits for the game client to end, then tells the window (WM_GAME_EXITED), so
// LAUNCH GAME is on again.  Takes over the process handle.
static void watch_game(HWND hwnd, HANDLE process)
{
    std::thread([hwnd, process] {
        ::WaitForSingleObject(process, INFINITE);
        ::CloseHandle(process);
        ::PostMessageW(hwnd, WM_GAME_EXITED, 0, 0);
    }).detach();
}

// ===========================================================================
//  Auth Worker Thread
// ===========================================================================
// The auth server this run talks to: config\server.json (the start script
// puts its own copy next to the exe).  Read once per sign-in, not once per request.
static aoc::launcher::AuthEndpoint read_auth_endpoint()
{
    return aoc::launcher::auth_endpoint(load_server_json());
}

// One login or register request to `server`, and what the auth server answered.
// Every copy of the password made here (the request body) is wiped after it is sent.
static aoc::launcher::Reply request_token(const aoc::launcher::AuthEndpoint& server,
                                          const std::string& user, const std::string& pass, bool is_reg)
{
    const std::string path = is_reg ? "/api/v1/auth/register" : "/api/v1/auth/login";

    std::string body = aoc::launcher::login_body(user, pass);
    const aoc::launcher::ReadResult got = http_post(server, path, body);
    aoc::launcher::wipe(body);

    return aoc::launcher::reply_from_read(got);
}

// Hands the token to tether_server, which gives it to the game client as its
// ICS auth token (services/tether/ics_token_file.h).  A sign-in by hand and the
// automatic one write it the same way.
static bool store_token(const std::string& token)
{
    return aoc::ics_token::write(aoc::ics_token::path_in(exe_dir()), token);
}

static void auth_worker(HWND hwnd, std::wstring wuser, std::wstring wpass, bool is_reg, bool keep)
{
    namespace sign_in = aoc::launcher;
    const std::string user = to_utf8(wuser.c_str());
    std::string pass = to_utf8(wpass.c_str());
    const sign_in::Reply reply = request_token(read_auth_endpoint(), user, pass, is_reg);
    sign_in::wipe(pass);   // the UTF-8 copy; the request body was wiped in request_token

    auto* r = new AuthResult{};
    r->username = user;
    if (reply.kind != sign_in::ReplyKind::Token) {
        r->ok = false;
        r->message = reply.message;
    } else if (!store_token(reply.token)) {
        r->ok = false;
        r->message = sign_in::kMsgNotPassedOn;
    } else {
        r->ok = true;
        r->message = is_reg ? "Registered and signed in as " + user
                            : "Successfully signed in as " + user;
        // Keep me signed in: a sign-in by hand that worked saves (box ticked) or
        // forgets (box not ticked) the saved sign-in.
        switch (sign_in::remember_action(false, true, keep)) {
        case sign_in::Remember::Save:
            if (!sign_in::credential_save(sign_in::kCredentialTarget, sign_in::SavedSignIn{wuser, wpass}))
                r->note = "Signed in, but Windows would not save the sign-in.";
            break;
        case sign_in::Remember::Delete:
            sign_in::credential_remove(sign_in::kCredentialTarget);
            break;
        case sign_in::Remember::Leave:
            break;
        }
    }
    if (!wpass.empty()) SecureZeroMemory(wpass.data(), wpass.size() * sizeof(wchar_t));
    ::PostMessageW(hwnd, WM_AUTH_DONE, 0, (LPARAM)r);
    g_authBusy.store(false);
}

// The automatic sign-in at start (launcher_sign_in.h: auto_sign_in), on its own
// thread.  It talks to the window with two messages: WM_AUTO_STATUS (the small
// line) and, once, WM_AUTO_DONE.
static void auto_worker(HWND hwnd, aoc::launcher::SavedSignIn saved, aoc::launcher::AuthEndpoint server)
{
    namespace sign_in = aoc::launcher;
    sign_in::AutoOps ops;
    ops.cancelled = [] { return g_autoCancel.load(); };
    ops.show = [hwnd](const std::string& s) {
        auto* w = new std::wstring(to_wide(s));
        if (!::PostMessageW(hwnd, WM_AUTO_STATUS, 0, (LPARAM)w)) delete w;
    };
    ops.sleep = [](sign_in::Ms ms) { std::this_thread::sleep_for(ms); };
    ops.now = [] {
        return std::chrono::duration_cast<sign_in::Ms>(std::chrono::steady_clock::now().time_since_epoch());
    };
    // Every request, the retries too, goes to `server`: the host and port that were
    // read and checked once when this sign-in began.
    ops.request = [server](const sign_in::SavedSignIn& s) {
        std::string pass = to_utf8(s.password.c_str());
        const sign_in::Reply r = request_token(server, to_utf8(s.username.c_str()), pass, false);
        sign_in::wipe(pass);   // the UTF-8 copy; the request body was wiped in request_token
        return r;
    };
    ops.store_token = [](const std::string& token) { return store_token(token); };

    const sign_in::AutoResult res = sign_in::auto_sign_in(saved, ops);

    auto* d = new AutoDone{res.outcome, res.message, saved.username};
    if (!::PostMessageW(hwnd, WM_AUTO_DONE, 0, (LPARAM)d)) delete d;
}

// Starts the automatic sign-in for a saved sign-in, if the auth server is on
// this PC; otherwise the form stays and says why.
static void begin_auto_sign_in(HWND hwnd, const aoc::launcher::SavedSignIn& saved)
{
    // config\server.json is read here, once.  The server that is checked is the
    // server the thread gets, for every request and every retry.
    const std::optional<aoc::launcher::AuthEndpoint> server =
        aoc::launcher::auto_sign_in_endpoint(load_server_json());
    if (!server) {
        g_feedback = L"Automatic sign-in only works with a server on this PC.";
        g_feedbackErr = true;
        return;
    }
    g_autoActive = true;
    g_autoUser = saved.username;
    g_autoStatus.clear();
    g_autoCancel.store(false);
    std::thread([hwnd, saved, endpoint = *server] { auto_worker(hwnd, saved, endpoint); }).detach();
}
// Cyb + C.O.A

// ===========================================================================
//  Remembered User Persistence
// ===========================================================================
constexpr int kUserMaxChars = 64;

// %APPDATA%\<folder>\launcher.json ("" when APPDATA is not set).
static std::filesystem::path remembered_user_path(const wchar_t* folder = L"SandalCore")
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf) / folder / L"launcher.json";
}

// An optional header that is not part of every checkout: the names an earlier
// version of the launcher saved its state under (kLegacyUserFolder, the folder
// under %APPDATA%, and kLegacyCredentialTarget).  Where it exists, the first
// start moves what was saved under them to the current names; where it does not,
// nothing is moved.
#if __has_include("launcher_legacy_names.h")
#  include "launcher_legacy_names.h"
#endif

#ifdef AOC_LAUNCHER_LEGACY_NAMES
// Once, silently, at the start: the remembered user name and the saved sign-in of
// an earlier version move to the current names, and only where the current names
// hold nothing yet.  The old user name file stays; the old credential is deleted
// once the new one is saved.  Nothing is printed or logged.
static void move_legacy_state()
{
    aoc::launcher::copy_if_missing(remembered_user_path(aoc::launcher::kLegacyUserFolder), remembered_user_path());
    aoc::launcher::credential_move(aoc::launcher::kLegacyCredentialTarget, aoc::launcher::kCredentialTarget);
}
#endif

static std::wstring load_remembered_user()
{
    try {
        const auto path = remembered_user_path();
        if (path.empty()) return {};
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        const json j = json::parse(f);
        if (!j.is_object() || !j.contains("username") || !j["username"].is_string())
            return {};
        const std::wstring name = to_wide(j["username"].get<std::string>());
        if (name.empty() || name.size() > (size_t)kUserMaxChars) return {};
        for (const wchar_t c : name) if (c < L' ') return {};
        return name;
    } catch (...) { return {}; }
}

static void save_remembered_user(const std::string& name)
{
    if (name.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    try {
        const fs::path path = remembered_user_path();
        if (path.empty()) return;
        fs::create_directories(path.parent_path(), ec);
        fs::path tmp = path; tmp += L".tmp";

        json j; j["username"] = name;
        const std::string text = j.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << text;
            f.flush();
            f.close();
            if (!f) { fs::remove(tmp, ec); return; }
        }
        fs::rename(tmp, path, ec);
        if (ec) fs::remove(tmp, ec);
    } catch (...) {}
}

// ===========================================================================
//  Drawing & Rendering Helpers
// ===========================================================================
static void fill_rect(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r{ x, y, x + w, y + h };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &r, br);
    DeleteObject(br);
}
// [C-Y-B - CoA]

static void draw_text_ex(HDC dc, const wchar_t* txt, int x, int y, int w, int h,
                         HFONT fnt, COLORREF col, UINT flags)
{
    RECT r{ x, y, x + w, y + h };
    SelectObject(dc, fnt);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, txt, -1, &r, flags);
}

static void draw_rounded_btn(HDC dc, RECT r, const wchar_t* txt, HFONT fnt,
                             bool hover, bool pressed, bool enabled,
                             COLORREF baseBg, COLORREF hvBg, COLORREF psBg, COLORREF dsBg,
                             COLORREF txtCol = kTxtWht, COLORREF bdrCol = 0, bool hasBdr = false)
{
    COLORREF bg = !enabled ? dsBg : pressed ? psBg : hover ? hvBg : baseBg;
    HBRUSH br = CreateSolidBrush(bg);
    HPEN pen  = CreatePen(PS_SOLID, 1, (hasBdr && enabled) ? (hover ? kAccentHv : bdrCol) : bg);

    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN op   = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);

    SelectObject(dc, fnt);
    SetTextColor(dc, enabled ? txtCol : kTxtDim);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void draw_focus_mark(HDC dc, RECT r, COLORREF col)
{
    InflateRect(&r, -3, -3);
    HPEN   pen = CreatePen(PS_SOLID, 1, col);
    HPEN   op  = (HPEN)SelectObject(dc, pen);
    HBRUSH ob  = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 4, 4);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen);
}

static void draw_field_outline(HDC dc, int x, int y, int w, int h)
{
    HPEN pen = CreatePen(PS_SOLID, 1, kFieldBdr);
    HPEN op  = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, x - 1, y - 1, x + w + 1, y + h + 1, 4, 4);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen);
}

// ===========================================================================
//  WM_PAINT
// ===========================================================================
static void render_launcher(HWND hwnd, HDC dc)
{
    // Clean unified dark background (no black borders)
    fill_rect(dc, 0, 0, kW, kH, kBg);

    // ── Box 1: Account Login / Session / "Signing in as ..." ────────────────
    if (g_autoActive) {
        draw_text_ex(dc, L"SIGNING IN", 18, 14, 260, 20, g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE);

        std::wstring sUser = L"Signing in as  " + g_autoUser + L"\u2026";
        draw_text_ex(dc, sUser.c_str(), 18, kSessLineY, kW - 36, 26, g_fntBig, kTxtWht, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        draw_text_ex(dc, g_autoStatus.c_str(), 18, kSessNoteY, kW - 36, 18, g_fntSmall, kTxtSub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (!g_loggedIn) {
        draw_text_ex(dc, L"ACCOUNT LOGIN", 18, 14, 200, 20, g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE);

        draw_text_ex(dc, L"USERNAME", 18, 38, 120, 16, g_fntLabel, kTxtSub, DT_LEFT | DT_SINGLELINE);
        draw_field_outline(dc, 18, 54, kW - 36, 26);

        draw_text_ex(dc, L"PASSWORD", 18, 86, 120, 16, g_fntLabel, kTxtSub, DT_LEFT | DT_SINGLELINE);
        draw_field_outline(dc, 18, 102, kW - 36, 26);
    } else {
        draw_text_ex(dc, L"ACCOUNT", 18, 14, 260, 20, g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE);

        std::wstring sUser = L"\u2713  Signed in as  " + g_username;
        draw_text_ex(dc, sUser.c_str(), 18, kSessLineY + 12, kW - 36, 26, g_fntBig, kTxtOk,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // Feedback message line
    if (!g_feedback.empty()) {
        const int fbY = (g_loggedIn || g_autoActive) ? kSessFbY : kFormFbY;
        draw_text_ex(dc, g_feedback.c_str(), 18, fbY, kW - 36, 18,
                     g_fntSmall, g_feedbackErr ? kTxtErr : kTxtOk,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // Divider line between Box 1 and Box 2
    fill_rect(dc, 0, kCard2Y - 1, kW, 1, kCardBdr);

    // ── Box 2: Game Client ──────────────────────────────────────────────────
    const bool exeOk = g_gameExeOk;
    draw_text_ex(dc, L"GAME CLIENT", 18, kCard2Y + 10, 100, 18, g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE);

    if (exeOk) {
        draw_text_ex(dc, L"\u2713  READY", 120, kCard2Y + 11, 100, 18, g_fntLabel, kTxtOk, DT_LEFT | DT_SINGLELINE);
    } else {
        draw_text_ex(dc, L"\u26a0  NOT FOUND", 120, kCard2Y + 11, 150, 18, g_fntLabel, kTxtErr, DT_LEFT | DT_SINGLELINE);
    }

    // Path display box
    const int pbx = 18, pby = kCard2Y + 34, pbw = 230, pbh = 32;
    fill_rect(dc, pbx, pby, pbw, pbh, kFieldBg);
    draw_field_outline(dc, pbx, pby, pbw, pbh);

    std::wstring pathStr = exeOk ? to_wide(g_gameExe) : L"No game selected (use the buttons \u2192)";
    draw_text_ex(dc, pathStr.c_str(), pbx + 8, pby + 4, pbw - 16, pbh - 8,
                 g_fntSmall, exeOk ? kTxtSub : kTxtDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Divider line between Box 2 and Box 3
    fill_rect(dc, 0, kCard3Y - 1, kW, 1, kCardBdr);
// C | Y | B / CoA

    // ── Box 3: Launch Game ──────────────────────────────────────────────────
    std::wstring launchHint;
    COLORREF hintCol = kTxtSub;
    if (!exeOk) {
        launchHint = L"\u26a0  The game was not found. Use Auto-Detect or Browse.";
        hintCol = kTxtErr;
    } else if (g_gameRunning) {
        launchHint = L"The game is running.";
        hintCol = kTxtSub;
    } else if (g_autoActive) {
        launchHint = L"Signing in with your saved sign-in\u2026";
        hintCol = kTxtSub;
    } else if (!g_loggedIn) {
        launchHint = L"Please sign in or register above to launch.";
        hintCol = kTxtWarn;
    } else {
        launchHint = L"\u2713  Ready to play.";
        hintCol = kTxtOk;
    }
    draw_text_ex(dc, launchHint.c_str(), 18, kCard3Y + 68, kW - 36, 18,
                 g_fntSmall, hintCol, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}


static void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    render_launcher(hwnd, dc);
    EndPaint(hwnd, &ps);
}


// ===========================================================================
//  WM_DRAWITEM (Custom Rounded Buttons)
// ===========================================================================

// The "Keep me signed in" box, drawn in the window's own colours (a system
// check box would be white on this dark window).
static void draw_keep_checkbox(DRAWITEMSTRUCT* dis)
{
    HDC dc = dis->hDC;
    const RECT r = dis->rcItem;
    const bool enabled = (dis->itemState & ODS_DISABLED) == 0;
    fill_rect(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, kBg);

    const int box = 14;
    const int bx = r.left + 6, by = r.top + ((r.bottom - r.top) - box) / 2;
    const COLORREF fill = g_keepSignedIn ? kAccent : kFieldBg;
    const COLORREF edge = g_keepSignedIn ? kAccent : (g_hoverKeep ? kAccentHv : kFieldBdr);
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pen = CreatePen(PS_SOLID, 1, edge);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN   op = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, bx, by, bx + box, by + box, 4, 4);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);

    if (g_keepSignedIn) {
        HPEN tick = CreatePen(PS_SOLID, 2, kTxtWht);
        HPEN old  = (HPEN)SelectObject(dc, tick);
        MoveToEx(dc, bx + 3, by + 7, nullptr);
        LineTo(dc, bx + 6, by + 10);
        LineTo(dc, bx + 11, by + 4);
        SelectObject(dc, old);
        DeleteObject(tick);
    }

    draw_text_ex(dc, L"Keep me signed in", bx + box + 8, r.top, r.right - (bx + box + 8), r.bottom - r.top,
                 g_fntSmall, !enabled ? kTxtDim : g_hoverKeep ? kTxtWht : kTxtSub,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void on_draw_item(DRAWITEMSTRUCT* dis)
{
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool enabled = (dis->itemState & ODS_DISABLED) == 0;
    RECT r = dis->rcItem;

    switch (dis->CtlID) {
    case ID_BROWSE:
        draw_rounded_btn(dis->hDC, r, L"Browse\u2026", g_fntMed,
                         g_hoverBrowse, pressed, enabled,
                         kBtnDark, kBtnDkHv, kBtnDkPs, kBtnDark,
                         kTxtWht, kCardBdr, true);
        break;

    case ID_AUTODETECT:
        draw_rounded_btn(dis->hDC, r, L"Auto-Detect", g_fntMed,
                         g_hoverAutoDetect, pressed, enabled,
                         kBtnDark, kBtnDkHv, kBtnDkPs, kBtnDark,
                         kTxtWht, kCardBdr, true);
        break;

    case ID_LOGIN:
        draw_rounded_btn(dis->hDC, r, L"Login", g_fntMed,
                         g_hoverLogin, pressed, enabled,
                         kAccent, kAccentHv, kAccentPs, kAccentDs);
        break;

    case ID_REG:
        draw_rounded_btn(dis->hDC, r, L"Register", g_fntMed,
                         g_hoverReg, pressed, enabled,
                         kBtnDark, kBtnDkHv, kBtnDkPs, kBtnDark,
                         kTxtWht, kCardBdr, true);
        break;

    case ID_LOGOUT:
        draw_rounded_btn(dis->hDC, r, L"Switch Account / Logout", g_fntMed,
                         g_hoverLogout, pressed, enabled,
                         kBtnDark, kBtnDkHv, kBtnDkPs, kBtnDark,
                         kTxtWht, kCardBdr, true);
        break;

    case ID_CANCEL:
        draw_rounded_btn(dis->hDC, r, L"Cancel", g_fntMed,
                         g_hoverCancel, pressed, enabled,
                         kBtnDark, kBtnDkHv, kBtnDkPs, kBtnDark,
                         kTxtWht, kCardBdr, true);
        break;
// C | O | A x C.Y.B

    case ID_KEEP:
        draw_keep_checkbox(dis);
        break;

    case ID_LAUNCH:
        draw_rounded_btn(dis->hDC, r, g_gameRunning ? L"GAME RUNNING" : L"LAUNCH GAME", g_fntBig,
                         g_hoverLaunch, pressed, enabled,
                         kAccent, kAccentHv, kAccentPs, kAccentDs);
        break;
    }

    if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT)) {
        draw_focus_mark(dis->hDC, r, (dis->CtlID == ID_LOGIN || dis->CtlID == ID_LAUNCH) ? kTxtWht : kAccentHv);
    }
}

// ===========================================================================
//  Capture to Bitmap File (Screenshots & Testing)
// ===========================================================================
static bool save_bitmap_to_file(HBITMAP hbm, int w, int h, const std::wstring& path)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h; // standard bottom-up DIB (GetDIBits rejects negative height)
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    const DWORD rowSize = ((w * 3 + 3) & ~3);
    const DWORD imageSize = rowSize * h;
    std::vector<BYTE> pixels(imageSize, 0);

    HDC screenDC = GetDC(nullptr);
    int lines = GetDIBits(screenDC, hbm, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screenDC);
    if (lines <= 0) return false;

    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42; // "BM"
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + imageSize;

    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    f.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(bi.bmiHeader));
    f.write(reinterpret_cast<const char*>(pixels.data()), imageSize);
    return true;
}

static bool capture_launcher_to_file(HWND hwnd, const std::wstring& path)
{
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hbm = CreateCompatibleBitmap(screenDC, kW, kH);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hbm);

    // 1. Render main surface
    render_launcher(hwnd, memDC);

    // 2. Render child controls onto memory DC
    HWND c = GetWindow(hwnd, GW_CHILD);
    while (c) {
        if (IsWindowVisible(c)) {
            RECT cr;
            GetWindowRect(c, &cr);
            POINT pt1{ cr.left, cr.top }, pt2{ cr.right, cr.bottom };
            ScreenToClient(hwnd, &pt1);
            ScreenToClient(hwnd, &pt2);
            RECT r{ pt1.x, pt1.y, pt2.x, pt2.y };

            const int id = GetDlgCtrlID(c);
            const LONG style = GetWindowLongW(c, GWL_STYLE);
// [C Y B - C-O-A]

            if ((style & BS_OWNERDRAW) == BS_OWNERDRAW) {
                DRAWITEMSTRUCT dis{};
                dis.CtlType = ODT_BUTTON;
                dis.CtlID = id;
                dis.hwndItem = c;
                dis.hDC = memDC;
                dis.rcItem = r;
                dis.itemState = IsWindowEnabled(c) ? 0 : ODS_DISABLED;
                on_draw_item(&dis);
            } else {
                fill_rect(memDC, r.left, r.top, r.right - r.left, r.bottom - r.top, kFieldBg);
                draw_field_outline(memDC, r.left, r.top, r.right - r.left, r.bottom - r.top);
                wchar_t buf[256]{};
                GetWindowTextW(c, buf, 256);
                if ((style & ES_PASSWORD) != 0) {
                    // never draw a typed password: one bullet per character
                    for (wchar_t* p = buf; *p; ++p) *p = L'\u2022';
                }
                if (buf[0] != L'\0') {
                    draw_text_ex(memDC, buf, r.left + 6, r.top + 4, r.right - r.left - 12, r.bottom - r.top - 8,
                                 g_fntForm, kTxtWht, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }
        c = GetWindow(c, GW_HWNDNEXT);
    }

    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    bool ok = save_bitmap_to_file(hbm, kW, kH, path);
    DeleteObject(hbm);
    return ok;
}

// ===========================================================================
//  Sync Control Visibility & State
// ===========================================================================
static void sync_controls(HWND hwnd)
{
    const bool lo = g_loggedIn;
    const bool form = !lo && !g_autoActive;   // the form shows unless signed in or the automatic sign-in runs

    ShowWindow(GetDlgItem(hwnd, ID_USER),   form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_PASS),   form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_KEEP),   form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_LOGIN),  form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_REG),    form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_LOGOUT), lo ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_CANCEL), g_autoActive ? SW_SHOW : SW_HIDE);

    // LAUNCH GAME: on when signed in and the game is found, off while the
    // client this window started still runs (it reads GAME RUNNING then).
    EnableWindow(GetDlgItem(hwnd, ID_LAUNCH), lo && g_gameExeOk && !g_gameRunning);

    InvalidateRect(hwnd, nullptr, FALSE);
}

// ===========================================================================
//  Keyboard Focus / Default Button
// ===========================================================================
static int default_button_id(HWND hwnd)
{
    // While "Signing in as ..." shows, a stray Enter does nothing (Esc and the
    // Cancel button cancel).
    int id = g_autoActive ? 0 : g_loggedIn ? ID_LAUNCH : ID_LOGIN;
    const HWND f = GetFocus();
    if (f && IsChild(hwnd, f)) {
        const int fid = GetDlgCtrlID(f);
        if (fid == ID_LOGIN || fid == ID_REG || fid == ID_LAUNCH ||
            fid == ID_BROWSE || fid == ID_AUTODETECT || fid == ID_LOGOUT || fid == ID_CANCEL) {
            id = fid;
        }
    }
    const HWND b = id ? GetDlgItem(hwnd, id) : nullptr;
    return (b && IsWindowVisible(b) && IsWindowEnabled(b)) ? id : 0;
}

static HWND default_focus(HWND hwnd)
{
    if (g_autoActive) return nullptr;   // no control takes the focus; Tab reaches Cancel
    if (g_loggedIn) {
        const HWND l = GetDlgItem(hwnd, ID_LAUNCH);
        return (IsWindowVisible(l) && IsWindowEnabled(l)) ? l : nullptr;
    }
    const HWND u = GetDlgItem(hwnd, ID_USER);
    return GetWindowTextLengthW(u) > 0 ? GetDlgItem(hwnd, ID_PASS) : u;
}

static void restore_focus(HWND hwnd)
{
    HWND t = g_lastFocus;
    if (!t || !IsChild(hwnd, t) || !IsWindowVisible(t) || !IsWindowEnabled(t))
        t = default_focus(hwnd);
    if (t) SetFocus(t);
}

static void focus_control(HWND hwnd, HWND c)
{
    if (!c) return;
    if (GetActiveWindow() == hwnd) SetFocus(c);
    else g_lastFocus = c;
}

// ===========================================================================
//  Window Procedure
// ===========================================================================
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_hwnd = hwnd;
        HINSTANCE hi = GetModuleHandleW(nullptr);

        // Game path from config\paths.json; if it is missing, look for Steam's
        // copy and save what was found.  Not with --screenshot (no file writes).
        set_game_exe(load_game_exe());
        if (!g_screenshotMode && !g_gameExeOk) {
            const std::string detected = auto_detect_game();
            if (!detected.empty()) {
                set_game_exe(detected);
                save_game_exe(detected);
                g_feedback = L"Found the game.";
                g_feedbackErr = false;
            }
        }
// built by CoA with C-Y-B

        // Account Input fields (Box 1)
        HWND hU = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            18, 54, kW - 36, 26,
            hwnd, (HMENU)(INT_PTR)ID_USER, hi, nullptr);
        SendMessageW(hU, EM_SETLIMITTEXT, kUserMaxChars, 0);

        HWND hP = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
            18, 102, kW - 36, 26,
            hwnd, (HMENU)(INT_PTR)ID_PASS, hi, nullptr);
        SendMessageW(hP, EM_SETLIMITTEXT, 128, 0);

        // "Keep me signed in" (drawn by draw_keep_checkbox; a click flips g_keepSignedIn)
        CreateWindowW(L"BUTTON", L"Keep me signed in",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            18, kKeepY - 2, 190, 24,
            hwnd, (HMENU)(INT_PTR)ID_KEEP, hi, nullptr);

        // Account Buttons
        const int authBtnY = kAuthBtnY;
        const int authBtnW = (kW - 36 - 10) / 2;
        CreateWindowW(L"BUTTON", L"Login",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
            18, authBtnY, authBtnW, 34,
            hwnd, (HMENU)(INT_PTR)ID_LOGIN, hi, nullptr);

        CreateWindowW(L"BUTTON", L"Register",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            18 + authBtnW + 10, authBtnY, authBtnW, 34,
            hwnd, (HMENU)(INT_PTR)ID_REG, hi, nullptr);

        // Switch Account / Logout button (shown when logged in)
        CreateWindowW(L"BUTTON", L"Switch Account / Logout",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            (kW - 210) / 2, kSessBtnY, 210, 34,
            hwnd, (HMENU)(INT_PTR)ID_LOGOUT, hi, nullptr);

        // Cancel button (shown while "Signing in as ..." counts down)
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            (kW - 210) / 2, kSessBtnY, 210, 34,
            hwnd, (HMENU)(INT_PTR)ID_CANCEL, hi, nullptr);

        // Browse & Auto-Detect buttons (Box 2)
        CreateWindowW(L"BUTTON", L"Browse\u2026",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            256, kCard2Y + 34, 84, 32,
            hwnd, (HMENU)(INT_PTR)ID_BROWSE, hi, nullptr);

        CreateWindowW(L"BUTTON", L"Auto-Detect",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            348, kCard2Y + 34, 84, 32,
            hwnd, (HMENU)(INT_PTR)ID_AUTODETECT, hi, nullptr);

        // LAUNCH GAME button (Box 3)
        const int launchW = kW - 36, launchH = 46;
        CreateWindowW(L"BUTTON", L"LAUNCH GAME",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED,
            18, kCard3Y + 14,
            launchW, launchH,
            hwnd, (HMENU)(INT_PTR)ID_LAUNCH, hi, nullptr);

        // Fonts
        SendMessageW(hU, WM_SETFONT, (WPARAM)g_fntForm, TRUE);
        SendMessageW(hP, WM_SETFONT, (WPARAM)g_fntForm, TRUE);

        if (g_screenshotMode) {
            // --screenshot [--preview-state <name>]: draws a made-up state.  It
            // reads no saved sign-in and no remembered user, and asks no server.
            if (g_previewState == L"signed-in") {
                g_loggedIn = true;
                g_username = L"Example";
            } else if (g_previewState == L"signing-in") {
                g_autoActive = true;
                g_autoUser = L"Example";
                g_autoStatus = L"Starting in 3 s. Press Cancel to use another account.";
            } else if (g_previewState == L"form-kept") {
                g_keepSignedIn = true;
            } else if (g_previewState == L"game-running") {
                g_loggedIn = true;
                g_username = L"Example";
                g_gameRunning = true;
            }
        } else {
            // The user remembered from the last sign-in; a saved sign-in (Keep me
            // signed in) names the account itself, and is signed in at once:
            // "Signing in as <name>..." with Cancel, then the token file.
#ifdef AOC_LAUNCHER_LEGACY_NAMES
            move_legacy_state();     // what an earlier version saved comes along, once
#endif
            std::wstring remembered = load_remembered_user();
            const auto saved = aoc::launcher::credential_load(aoc::launcher::kCredentialTarget);
            if (saved) {
                remembered = saved->username;
                g_keepSignedIn = true;
            }
            if (!remembered.empty()) SetWindowTextW(hU, remembered.c_str());
            if (saved) begin_auto_sign_in(hwnd, *saved);
        }

        sync_controls(hwnd);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            const HWND f = GetFocus();
            g_lastFocus = (f && IsChild(hwnd, f)) ? f : nullptr;
            break;
        }
        if (!IsIconic(hwnd)) {
            restore_focus(hwnd);
            return 0;
        }
        break;

    case WM_SETFOCUS:
        if (!IsIconic(hwnd)) restore_focus(hwnd);
        return 0;

    case DM_GETDEFID:
    {
        const int id = default_button_id(hwnd);
        return id ? MAKELRESULT(id, DC_HASDEFID) : 0;
    }

    case WM_PAINT:
        on_paint(hwnd);
        return 0;

    case WM_PRINT:
    case WM_PRINTCLIENT:
        render_launcher(hwnd, (HDC)wp);
        return 0;
// -- C O A x C_Y_B --

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT:
    {
        HDC dc = (HDC)wp;
        SetTextColor(dc, kTxtWht);
        SetBkColor(dc, kFieldBg);
        return (LRESULT)g_brField;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)wp;
        SetTextColor(dc, kTxtSub);
        SetBkColor(dc, kCardBg);
        return (LRESULT)g_brCard;
    }

    case WM_CTLCOLORBTN:
        return (LRESULT)g_brBg;

    case WM_DRAWITEM:
        on_draw_item((DRAWITEMSTRUCT*)lp);
        return TRUE;

    case WM_MOUSEMOVE:
    {
        const int mx = LOWORD(lp), my = (SHORT)HIWORD(lp);
        // Hidden controls share places with visible ones (Logout sits over the
        // form): skip them, or the wrong button would show as hovered.
        HWND hit = ChildWindowFromPointEx(hwnd, {mx, my}, CWP_SKIPINVISIBLE);

        auto refresh_btn = [&](HWND h, bool& flag, bool want) {
            if (flag != want) { flag = want; if (h) InvalidateRect(h, nullptr, FALSE); }
        };
        refresh_btn(GetDlgItem(hwnd, ID_BROWSE),     g_hoverBrowse,     hit == GetDlgItem(hwnd, ID_BROWSE));
        refresh_btn(GetDlgItem(hwnd, ID_AUTODETECT), g_hoverAutoDetect, hit == GetDlgItem(hwnd, ID_AUTODETECT));
        refresh_btn(GetDlgItem(hwnd, ID_LOGIN),      g_hoverLogin,      hit == GetDlgItem(hwnd, ID_LOGIN));
        refresh_btn(GetDlgItem(hwnd, ID_REG),        g_hoverReg,        hit == GetDlgItem(hwnd, ID_REG));
        refresh_btn(GetDlgItem(hwnd, ID_LOGOUT),     g_hoverLogout,     hit == GetDlgItem(hwnd, ID_LOGOUT));
        refresh_btn(GetDlgItem(hwnd, ID_LAUNCH),     g_hoverLaunch,     hit == GetDlgItem(hwnd, ID_LAUNCH));
        refresh_btn(GetDlgItem(hwnd, ID_KEEP),       g_hoverKeep,       hit == GetDlgItem(hwnd, ID_KEEP));
        refresh_btn(GetDlgItem(hwnd, ID_CANCEL),     g_hoverCancel,     hit == GetDlgItem(hwnd, ID_CANCEL));
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hoverBrowse = g_hoverAutoDetect = g_hoverLogin = g_hoverReg = g_hoverLogout = g_hoverLaunch = false;
        g_hoverKeep = g_hoverCancel = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_AUTH_DONE:
    {
        auto* r = (AuthResult*)lp;
        const bool ok = r->ok;
        if (ok) {
            g_loggedIn = true;
            g_username = to_wide(r->username);
            g_feedback = to_wide(r->note);        // "" unless the sign-in could not be saved
            g_feedbackErr = !r->note.empty();
            save_remembered_user(r->username);
        } else {
            g_feedbackErr = true;
            g_feedback = to_wide(r->message);
        }
        delete r;
        sync_controls(hwnd);
        if (ok) focus_control(hwnd, default_focus(hwnd));
        return 0;
    }

    // The automatic sign-in's small status line (a new std::wstring).
    case WM_AUTO_STATUS:
    {
        std::unique_ptr<std::wstring> line((std::wstring*)lp);
        if (g_autoActive) {                       // not after Cancel
            g_autoStatus = *line;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
// C-Y-B - C O A

    // The automatic sign-in is over (an AutoDone).  After Cancel the form has
    // been showing for a while and nothing here applies, except a sign-in that
    // had already written its token a moment before Cancel: the game starts
    // then, so the window says signed in.
    case WM_AUTO_DONE:
    {
        std::unique_ptr<AutoDone> d((AutoDone*)lp);
        if (!g_autoActive && d->outcome != aoc::launcher::AutoOutcome::SignedIn) return 0;
        g_autoActive = false;
        g_feedback.clear();
        g_feedbackErr = false;
        switch (d->outcome) {
        case aoc::launcher::AutoOutcome::SignedIn:
            g_loggedIn = true;
            g_username = d->username;
            save_remembered_user(to_utf8(d->username.c_str()));
            break;
        case aoc::launcher::AutoOutcome::Refused:
            // The saved sign-in stays: the account may exist in another
            // database, or the password was changed.  Signing in by hand with
            // the box ticked replaces it.
            g_feedback = L"Saved sign-in refused: " + to_wide(d->message) + L" Sign in again.";
            g_feedbackErr = true;
            break;
        case aoc::launcher::AutoOutcome::GaveUp:
            g_feedback = to_wide(d->message) + L" Try again when the server is running.";
            g_feedbackErr = true;
            break;
        case aoc::launcher::AutoOutcome::TokenNotSaved:
            g_feedback = to_wide(d->message);
            g_feedbackErr = true;
            break;
        case aoc::launcher::AutoOutcome::Cancelled:
            g_feedback = L"Automatic sign-in cancelled.";
            break;
        }
        sync_controls(hwnd);
        focus_control(hwnd, default_focus(hwnd));
        return 0;
    }

    // The game client this window started has ended: LAUNCH GAME is on again.
    case WM_GAME_EXITED:
        g_gameRunning = false;
        g_feedback.clear();
        g_feedbackErr = false;
        sync_controls(hwnd);
        return 0;

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == IDOK) id = default_button_id(hwnd);

        // Keep me signed in: the box
        if (id == ID_KEEP) {
            g_keepSignedIn = !g_keepSignedIn;
            InvalidateRect(GetDlgItem(hwnd, ID_KEEP), nullptr, FALSE);
            return 0;
        }

        // Cancel (the button, or Esc) during "Signing in as ...": the form shows
        // at once; the saved sign-in stays, and a token that is still on its way
        // is dropped (auto_sign_in checks g_autoCancel before it writes one).
        // Esc outside "Signing in as ..." does nothing.
        if (id == ID_CANCEL) {
            if (g_autoActive) {
                g_autoCancel.store(true);
                g_autoActive = false;
                g_feedback = L"Automatic sign-in cancelled.";
                g_feedbackErr = false;
                sync_controls(hwnd);
                focus_control(hwnd, default_focus(hwnd));
            }
            return 0;
        }

        // Browse Executable
        if (id == ID_BROWSE) {
            std::string picked = browse_for_game(hwnd, g_gameExe);
            if (!picked.empty()) {
                set_game_exe(picked);
                save_game_exe(picked);
                g_feedback = L"Game location updated.";
                g_feedbackErr = false;
                sync_controls(hwnd);
            }
            return 0;
        }

        // Auto-Detect Steam
        if (id == ID_AUTODETECT) {
            std::string found = auto_detect_game();
            if (!found.empty()) {
                set_game_exe(found);
                save_game_exe(found);
                g_feedback = L"Found the game on Steam.";
                g_feedbackErr = false;
            } else {
                g_feedback = L"Couldn't find the game. Please use Browse.";
                g_feedbackErr = true;
            }
            sync_controls(hwnd);
            return 0;
        }

        // Logout
        if (id == ID_LOGOUT) {
            // Switch Account / Logout forgets the saved sign-in: the next start
            // shows the form, with the username filled in.
            const bool wasKept = g_keepSignedIn;   // the box is ticked while a sign-in is saved
            const bool gone = aoc::launcher::credential_remove(aoc::launcher::kCredentialTarget);
            g_keepSignedIn = false;
            g_loggedIn = false;
            g_username.clear();
            g_feedback = !gone   ? L"Logged out, but Windows would not delete the saved sign-in."
                       : wasKept ? L"Logged out. The saved sign-in is forgotten."
                                 : L"Logged out.";
            g_feedbackErr = !gone;
            SetDlgItemTextW(hwnd, ID_PASS, L"");
            sync_controls(hwnd);
            focus_control(hwnd, GetDlgItem(hwnd, ID_USER));
            return 0;
        }

        // Login / Register
        if ((id == ID_LOGIN || id == ID_REG) && !g_authBusy.exchange(true)) {
            wchar_t wu[128]{}, wp2[128]{};
            GetDlgItemTextW(hwnd, ID_USER, wu, 128);
            GetDlgItemTextW(hwnd, ID_PASS, wp2, 128);
            std::wstring user = wu, pass = wp2;
            SecureZeroMemory(wp2, sizeof(wp2));

            if (user.empty() || pass.empty()) {
                aoc::launcher::wipe(pass);
                g_feedback = L"Username and password are required.";
                g_feedbackErr = true;
                g_authBusy.store(false);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }

            g_feedback = L"Signing in\u2026";
            g_feedbackErr = false;
            InvalidateRect(hwnd, nullptr, FALSE);
// -- C | O | A x Cyb --

            const bool is_reg = (id == ID_REG);
            const bool keep = g_keepSignedIn;     // the box as it was when the button was pressed
            // The thread gets its own copy of the password; every copy is wiped when
            // it is done with: auth_worker's, the thread's, and this one.
            std::thread([hwnd, user, pass, is_reg, keep]() mutable {
                auth_worker(hwnd, user, pass, is_reg, keep);
                aoc::launcher::wipe(pass);
            }).detach();
            aoc::launcher::wipe(pass);
        }

        // Launch Game: only the player's press starts the game (a sign-in, by
        // hand or by itself, never does), and never a second client.
        if (id == ID_LAUNCH && g_loggedIn) {
            if (g_gameRunning || aoc::launcher::game_is_running()) {
                g_feedback = L"The game is already running.";
                g_feedbackErr = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            g_feedback = L"Starting the game\u2026";
            g_feedbackErr = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);

            HANDLE process = nullptr;
            const std::string err = start_game(&process);
            if (err.empty()) {
                g_gameRunning = true;           // the button reads GAME RUNNING until the client ends
                g_feedback.clear();
                watch_game(hwnd, process);
            } else {
                g_feedback = to_wide(err);
                g_feedbackErr = true;
                MessageBoxA(hwnd, err.c_str(), "SandalCore", MB_OK | MB_ICONERROR);
            }
            sync_controls(hwnd);
        }
        return 0;
    }

    case WM_DESTROY:
        g_autoCancel.store(true);   // an automatic sign-in still running must not write a token after the window is gone
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
//  Command Line
// ===========================================================================
// Sets g_screenshotMode / g_previewState; returns the --screenshot file ("" when
// there is none).
static std::wstring parse_command_line()
{
    std::wstring screenshot;
    int n = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!argv) return screenshot;
    for (int i = 1; i < n; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--screenshot" && i + 1 < n)           screenshot = argv[++i];
        else if (a == L"--preview-state" && i + 1 < n)   g_previewState = argv[++i];
    }
    LocalFree(argv);
    g_screenshotMode = !screenshot.empty();
    return screenshot;
}

// ===========================================================================
//  WinMain Entry Point
// ===========================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    const std::wstring screenshotPath = parse_command_line();

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    g_brBg    = CreateSolidBrush(kBg);
    g_brCard  = CreateSolidBrush(kCardBg);
    g_brField = CreateSolidBrush(kFieldBg);

    g_fntHeader = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fntMed    = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fntBig    = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fntForm   = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fntLabel  = CreateFontW(11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fntSmall  = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    WNDCLASSW wc{};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"SandalCoreLauncher";
    wc.hbrBackground = g_brBg;
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon         = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr{ 0, 0, kW, kH };
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    const int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    // WS_EX_CONTROLPARENT: the window's children are tab stops (see the loop below).
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT,
        L"SandalCoreLauncher",
        L"SandalCore \u2014 Ashes of Creation Emulator",
        style | WS_VISIBLE,
        sx, sy, ww, wh, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    int exitCode = 0;
    if (g_screenshotMode) {
        // --screenshot <file>: draw the window and leave (headless preview).
        exitCode = capture_launcher_to_file(hwnd, screenshotPath) ? 0 : 2;
        DestroyWindow(hwnd);
    } else {
        // The main window is a plain window, not a dialog: IsDialogMessage gives
        // it the dialog keys (Tab moves between the WS_TABSTOP controls, Enter
        // presses the default button via DM_GETDEFID).  Whatever it does not take
        // is translated and dispatched as usual.
        MSG m{};
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            // A held-down Enter must not repeat into the default button (Login
            // would be followed by LAUNCH GAME as soon as the login succeeds).
            if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && (m.lParam & (1 << 30)))
                continue;
            if (!IsDialogMessageW(hwnd, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        exitCode = (int)m.wParam;
    }

    DeleteObject(g_fntHeader);
    DeleteObject(g_fntMed);
    DeleteObject(g_fntBig);
    DeleteObject(g_fntForm);
    DeleteObject(g_fntLabel);
    DeleteObject(g_fntSmall);
    DeleteObject(g_brBg);
    DeleteObject(g_brCard);
    DeleteObject(g_brField);
    return exitCode;
}

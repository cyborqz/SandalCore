// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ===========================================================================
//  launcher_main.cpp  –  Sandal Core Emulator Launcher (Modern UI)
//
//  Win32 GUI. No gRPC/protobuf — WinInet + nlohmann_json only.
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
#include "services/tether/ics_token_file.h"

#include <atomic>
#include <filesystem>
#include <fstream>
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

// ===========================================================================
//  Window & Box Layout Dimensions (Only Login, Client Path, Launch Game)
// ===========================================================================
constexpr int kW = 450;

constexpr int kCardX   = 0;
constexpr int kCardW   = kW;
constexpr int kCard1Y  = 0;
constexpr int kCard1H  = 192; // Box 1: Account Login
constexpr int kCard2Y  = 192;
constexpr int kCard2H  = 82;  // Box 2: Client path
constexpr int kCard3Y  = 274;
constexpr int kCard3H  = 96;  // Box 3: Launch Game

constexpr int kH       = 370;

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
};

// ===========================================================================
//  GDI Resources
// ===========================================================================
static HBRUSH g_brBg      = nullptr;
static HBRUSH g_brCard    = nullptr;
static HBRUSH g_brField   = nullptr;
static HFONT  g_fntTitle  = nullptr;
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

// Hover flags
static bool g_hoverLogin      = false;
static bool g_hoverReg        = false;
static bool g_hoverLaunch     = false;
static bool g_hoverBrowse     = false;
static bool g_hoverAutoDetect = false;
static bool g_hoverLogout     = false;

static std::atomic<bool> g_authBusy{false};
constexpr UINT WM_AUTH_DONE = WM_APP + 1;
struct AuthResult { bool ok; std::string message; std::string username; };

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

static std::string exe_dir()
{
    char buf[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    auto p = s.find_last_of("\\/");
    return (p != std::string::npos) ? s.substr(0, p + 1) : "";
}

static std::string json_str(const std::string& path, const std::string& key,
                            const std::string& def = "")
{
    try {
        std::ifstream f(path); if (!f) return def;
        return json::parse(f).value(key, def);
    } catch (...) { return def; }
}

static int json_int(const std::string& path, const std::string& key, int def)
{
    try {
        std::ifstream f(path); if (!f) return def;
        return json::parse(f).value(key, def);
    } catch (...) { return def; }
}

// ===========================================================================
//  Paths Configuration Persistence
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

static std::string load_game_exe()
{
    for (const auto& p : config_paths_candidates()) {
        if (std::filesystem::exists(p)) {
            std::string exe = json_str(p, "game_executable", "");
            if (!exe.empty() && std::filesystem::exists(exe)) return exe;
        }
    }
    for (const auto& p : config_paths_candidates()) {
        if (std::filesystem::exists(p)) {
            std::string exe = json_str(p, "game_executable", "");
            if (!exe.empty()) return exe;
        }
    }
    return json_str(exe_dir() + "config\\paths.json", "game_executable", "");
}

static bool save_paths_json(const std::string& exe_path)
{
    bool saved = false;
    for (const auto& p : config_paths_candidates()) {
        try {
            std::filesystem::path fp(p);
            if (std::filesystem::exists(fp.parent_path().parent_path()) || std::filesystem::exists(fp.parent_path())) {
                std::filesystem::create_directories(fp.parent_path());
                json j;
                std::ifstream in(p);
                if (in) {
                    try { j = json::parse(in); } catch (...) {}
                    in.close();
                }
                j["game_executable"] = exe_path;
                std::ofstream out(p, std::ios::trunc);
                if (out) {
                    out << j.dump(4) << "\n";
                    saved = true;
                }
            }
        } catch (...) {}
    }
    return saved;
}

// ===========================================================================
//  Auto-Detect Steam & Ashes of Creation Executable
// ===========================================================================
static std::string auto_detect_game()
{
    std::vector<std::string> searchRoots;

    auto queryRegString = [](HKEY root, const wchar_t* subkey, const wchar_t* valName) -> std::string {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t buf[MAX_PATH] = { 0 };
            DWORD len = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExW(hKey, valName, nullptr, &type, (LPBYTE)buf, &len) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ)) {
                RegCloseKey(hKey);
                return to_utf8(buf);
            }
            RegCloseKey(hKey);
        }
        return "";
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

    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (drives & (1 << i)) {
            char dLetter = 'A' + i;
            std::string d = std::string(1, dLetter) + ":";
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
            try {
                if (std::filesystem::exists(full)) {
                    if (full.size() >= 13 && full.ends_with("AOCClient.exe")) {
                        std::string ship = full.substr(0, full.size() - 13) + "AOCClient-Win64-Shipping.exe";
                        if (std::filesystem::exists(ship)) return ship;
                    }
                    return full;
                }
            } catch (...) {}
        }
    }

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
            if (std::filesystem::exists(p.parent_path())) {
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
                if (std::filesystem::exists(ship)) {
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
static std::string http_post(const std::string& host, int port,
                             const std::string& path, const std::string& body)
{
    HINTERNET hI = InternetOpenA("AoCEmu/3.0", INTERNET_OPEN_TYPE_DIRECT,
                                 nullptr, nullptr, 0);
    if (!hI) return "";
    HINTERNET hC = InternetConnectA(hI, host.c_str(), (INTERNET_PORT)port,
                                    nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hC) { InternetCloseHandle(hI); return ""; }
    HINTERNET hR = HttpOpenRequestA(hC, "POST", path.c_str(), nullptr, nullptr,
                                    nullptr, INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hR) { InternetCloseHandle(hC); InternetCloseHandle(hI); return ""; }

    const char* hdrs = "Content-Type: application/json\r\n";
    std::string result;
    if (HttpSendRequestA(hR, hdrs, (DWORD)strlen(hdrs),
                         const_cast<char*>(body.c_str()), (DWORD)body.size())) {
        char buf[4096]; DWORD n;
        while (InternetReadFile(hR, buf, sizeof(buf)-1, &n) && n) {
            buf[n] = 0; result += buf;
        }
    }
    InternetCloseHandle(hR); InternetCloseHandle(hC); InternetCloseHandle(hI);
    return result;
}

// ===========================================================================
//  Game Launcher Process Execution
// ===========================================================================
static std::string launch_game()
{
    const std::string exe = g_gameExe;
    if (exe.empty())
        return "No game executable configured. Use Auto-Detect or Browse.";
    if (!std::filesystem::exists(exe))
        return "Executable not found at:\n" + exe;

    const std::string dir = exe_dir();
    const int tport = json_int(dir + "config\\server.json", "tether_port", 19021);
    const std::string extra = json_str(dir + "config\\server.json", "client_extra_args", "");

    std::string cmd = "\"" + exe + "\" -LauncherTetherPort=" + std::to_string(tport);
    if (!extra.empty()) cmd += ' ' + extra;

    std::string wd = exe;
    { auto p = wd.find_last_of("\\/"); if (p != std::string::npos) wd.resize(p+1); }

    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr,
                          FALSE, 0, nullptr,
                          wd.empty() ? nullptr : wd.c_str(), &si, &pi)) {
        return "CreateProcess failed (error " + std::to_string(::GetLastError()) + ")";
    }

    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return "";
}

// ===========================================================================
//  Auth Worker Thread
// ===========================================================================
static void auth_worker(HWND hwnd, std::string user, std::string pass, bool is_reg)
{
    const std::string dir = exe_dir();
    const std::string host = json_str(dir + "config\\server.json", "auth_server_host", "127.0.0.1");
    const int port = json_int(dir + "config\\server.json", "auth_server_port", 8081);
    const std::string ep = is_reg ? "/api/v1/auth/register" : "/api/v1/auth/login";

    json bj; bj["username"] = user; bj["password"] = pass;
    const std::string resp = http_post(host, port, ep, bj.dump());

    auto* r = new AuthResult{};
    r->username = user;
    if (resp.empty()) {
        r->ok = false;
        r->message = "Cannot connect to auth server. Ensure services are running.";
    } else {
        try {
            auto j = json::parse(resp);
            if (j.contains("access_token")) {
                const std::string token =
                    j["access_token"].is_string() ? j["access_token"].get<std::string>() : std::string{};
                if (token.empty()) {
                    r->ok = false;
                    r->message = "Auth server reply has an empty access_token.";
                } else if (!aoc::ics_token::write(aoc::ics_token::path_in(dir), token)) {
                    r->ok = false;
                    r->message = "Failed to save session token to data\\ics_auth_token.txt.";
                } else {
                    r->ok = true;
                    r->message = is_reg ? "Registered and signed in as " + user
                                        : "Successfully signed in as " + user;
                }
            } else {
                r->ok = false;
                r->message = j.value("message", j.value("error", "Authentication failed."));
            }
        } catch (...) {
            r->ok = false;
            r->message = "Unexpected response from auth server.";
        }
    }
    ::PostMessageW(hwnd, WM_AUTH_DONE, 0, (LPARAM)r);
    g_authBusy.store(false);
}

// ===========================================================================
//  Remembered User Persistence
// ===========================================================================
constexpr int kUserMaxChars = 64;

static std::filesystem::path remembered_user_path()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"AoCRiseOfPhoenix" / L"launcher.json";
}

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

static void draw_rounded_card(HDC dc, int x, int y, int w, int h, COLORREF bg, COLORREF bdr)
{
    HBRUSH br = CreateSolidBrush(bg);
    HPEN pen  = CreatePen(PS_SOLID, 1, bdr);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN op   = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, x, y, x + w, y + h, 8, 8);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

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

    // ── Box 1: Account Login / Session ──────────────────────────────────────
    if (!g_loggedIn) {
        draw_text_ex(dc, L"ACCOUNT LOGIN", 18, 14, 200, 20, g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE);

        draw_text_ex(dc, L"USERNAME", 18, 38, 120, 16, g_fntLabel, kTxtSub, DT_LEFT | DT_SINGLELINE);
        draw_field_outline(dc, 18, 54, kW - 36, 26);

        draw_text_ex(dc, L"PASSWORD", 18, 86, 120, 16, g_fntLabel, kTxtSub, DT_LEFT | DT_SINGLELINE);
        draw_field_outline(dc, 18, 102, kW - 36, 26);
    } else {
        draw_text_ex(dc, L"AUTHENTICATED SESSION", 18, 14, 260, 20, g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE);

        std::wstring sUser = L"\u2713  Signed in as  " + g_username;
        draw_text_ex(dc, sUser.c_str(), 18, 52, kW - 36, 26, g_fntBig, kTxtOk, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        draw_text_ex(dc, L"Session token active in data\\ics_auth_token.txt",
                     18, 80, kW - 36, 18, g_fntSmall, kTxtDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Feedback message line
    if (!g_feedback.empty()) {
        const int fbY = g_loggedIn ? 154 : 174;
        draw_text_ex(dc, g_feedback.c_str(), 18, fbY, kW - 36, 18,
                     g_fntSmall, g_feedbackErr ? kTxtErr : kTxtOk, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Divider line between Box 1 and Box 2
    fill_rect(dc, 0, kCard2Y - 1, kW, 1, kCardBdr);

    // ── Box 2: Game Client ──────────────────────────────────────────────────
    const bool exeOk = !g_gameExe.empty() && std::filesystem::exists(g_gameExe);
    draw_text_ex(dc, L"GAME CLIENT", 18, kCard2Y + 10, 100, 18, g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE);

    if (exeOk) {
        draw_text_ex(dc, L"\u2713  READY", 120, kCard2Y + 11, 100, 18, g_fntLabel, kTxtOk, DT_LEFT | DT_SINGLELINE);
    } else {
        draw_text_ex(dc, L"\u26a0  NOT CONFIGURED", 120, kCard2Y + 11, 150, 18, g_fntLabel, kTxtErr, DT_LEFT | DT_SINGLELINE);
    }

    // Path display box
    const int pbx = 18, pby = kCard2Y + 34, pbw = 230, pbh = 32;
    fill_rect(dc, pbx, pby, pbw, pbh, kFieldBg);
    draw_field_outline(dc, pbx, pby, pbw, pbh);

    std::wstring pathStr = exeOk ? to_wide(g_gameExe) : L"No client selected (use buttons \u2192)";
    draw_text_ex(dc, pathStr.c_str(), pbx + 8, pby + 4, pbw - 16, pbh - 8,
                 g_fntSmall, exeOk ? kTxtSub : kTxtDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Divider line between Box 2 and Box 3
    fill_rect(dc, 0, kCard3Y - 1, kW, 1, kCardBdr);

    // ── Box 3: Launch Game ──────────────────────────────────────────────────
    std::wstring launchHint;
    COLORREF hintCol = kTxtSub;
    if (!exeOk) {
        launchHint = L"\u26a0  Game client executable not found. Use Auto-Detect or Browse.";
        hintCol = kTxtErr;
    } else if (!g_loggedIn) {
        launchHint = L"Please sign in or register above to launch.";
        hintCol = kTxtWarn;
    } else {
        launchHint = L"\u2713  Ready to connect to emulator world server on port 7777.";
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

    case ID_LAUNCH:
        draw_rounded_btn(dis->hDC, r, L"LAUNCH GAME", g_fntBig,
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

    ShowWindow(GetDlgItem(hwnd, ID_USER),   lo ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, ID_PASS),   lo ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, ID_LOGIN),  lo ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, ID_REG),    lo ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, ID_LOGOUT), lo ? SW_SHOW : SW_HIDE);

    const bool canLaunch = lo && !g_gameExe.empty() && std::filesystem::exists(g_gameExe);
    EnableWindow(GetDlgItem(hwnd, ID_LAUNCH), canLaunch);

    InvalidateRect(hwnd, nullptr, FALSE);
}

// ===========================================================================
//  Keyboard Focus / Default Button
// ===========================================================================
static int default_button_id(HWND hwnd)
{
    int id = g_loggedIn ? ID_LAUNCH : ID_LOGIN;
    const HWND f = GetFocus();
    if (f && IsChild(hwnd, f)) {
        const int fid = GetDlgCtrlID(f);
        if (fid == ID_LOGIN || fid == ID_REG || fid == ID_LAUNCH ||
            fid == ID_BROWSE || fid == ID_AUTODETECT || fid == ID_LOGOUT) {
            id = fid;
        }
    }
    const HWND b = GetDlgItem(hwnd, id);
    return (b && IsWindowVisible(b) && IsWindowEnabled(b)) ? id : 0;
}

static HWND default_focus(HWND hwnd)
{
    if (g_loggedIn) {
        const HWND l = GetDlgItem(hwnd, ID_LAUNCH);
        return IsWindowEnabled(l) ? l : nullptr;
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

        // Load configured path from paths.json
        g_gameExe = load_game_exe();
        if (g_gameExe.empty() || !std::filesystem::exists(g_gameExe)) {
            std::string detected = auto_detect_game();
            if (!detected.empty()) {
                g_gameExe = detected;
                save_paths_json(g_gameExe);
                g_feedback = L"Auto-detected client: " + to_wide(g_gameExe);
                g_feedbackErr = false;
            }
        } else {
            save_paths_json(g_gameExe);
        }

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

        // Account Buttons
        const int authBtnY = 138;
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
            (kW - 210) / 2, 114, 210, 34,
            hwnd, (HMENU)(INT_PTR)ID_LOGOUT, hi, nullptr);

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

        // Load remembered user
        const std::wstring remembered = load_remembered_user();
        if (!remembered.empty()) SetWindowTextW(hU, remembered.c_str());

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
        HWND hit = ChildWindowFromPoint(hwnd, {mx, my});

        auto refresh_btn = [&](HWND h, bool& flag, bool want) {
            if (flag != want) { flag = want; if (h) InvalidateRect(h, nullptr, FALSE); }
        };
        refresh_btn(GetDlgItem(hwnd, ID_BROWSE),     g_hoverBrowse,     hit == GetDlgItem(hwnd, ID_BROWSE));
        refresh_btn(GetDlgItem(hwnd, ID_AUTODETECT), g_hoverAutoDetect, hit == GetDlgItem(hwnd, ID_AUTODETECT));
        refresh_btn(GetDlgItem(hwnd, ID_LOGIN),      g_hoverLogin,      hit == GetDlgItem(hwnd, ID_LOGIN));
        refresh_btn(GetDlgItem(hwnd, ID_REG),        g_hoverReg,        hit == GetDlgItem(hwnd, ID_REG));
        refresh_btn(GetDlgItem(hwnd, ID_LOGOUT),     g_hoverLogout,     hit == GetDlgItem(hwnd, ID_LOGOUT));
        refresh_btn(GetDlgItem(hwnd, ID_LAUNCH),     g_hoverLaunch,     hit == GetDlgItem(hwnd, ID_LAUNCH));
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hoverBrowse = g_hoverAutoDetect = g_hoverLogin = g_hoverReg = g_hoverLogout = g_hoverLaunch = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_AUTH_DONE:
    {
        auto* r = (AuthResult*)lp;
        const bool ok = r->ok;
        if (ok) {
            g_loggedIn = true;
            g_username = to_wide(r->username);
            g_feedback.clear();
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

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == IDOK) id = default_button_id(hwnd);

        // Browse Executable
        if (id == ID_BROWSE) {
            std::string picked = browse_for_game(hwnd, g_gameExe);
            if (!picked.empty()) {
                g_gameExe = picked;
                save_paths_json(g_gameExe);
                g_feedback = L"Game path updated.";
                g_feedbackErr = false;
                sync_controls(hwnd);
            }
            return 0;
        }

        // Auto-Detect Steam
        if (id == ID_AUTODETECT) {
            std::string found = auto_detect_game();
            if (!found.empty()) {
                g_gameExe = found;
                save_paths_json(g_gameExe);
                g_feedback = L"Found Steam client: " + to_wide(g_gameExe);
                g_feedbackErr = false;
            } else {
                g_feedback = L"Could not auto-detect Steam client. Please use Browse.";
                g_feedbackErr = true;
            }
            sync_controls(hwnd);
            return 0;
        }

        // Logout
        if (id == ID_LOGOUT) {
            g_loggedIn = false;
            g_username.clear();
            g_feedback = L"Logged out.";
            g_feedbackErr = false;
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
            std::string user = to_utf8(wu), pass = to_utf8(wp2);

            if (user.empty() || pass.empty()) {
                g_feedback = L"Username and password are required.";
                g_feedbackErr = true;
                g_authBusy.store(false);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }

            g_feedback = L"Contacting auth server\u2026";
            g_feedbackErr = false;
            InvalidateRect(hwnd, nullptr, FALSE);

            const bool is_reg = (id == ID_REG);
            std::thread([hwnd, user, pass, is_reg]{
                auth_worker(hwnd, user, pass, is_reg);
            }).detach();
        }

        // Launch Game
        if (id == ID_LAUNCH && g_loggedIn) {
            EnableWindow(GetDlgItem(hwnd, ID_LAUNCH), FALSE);
            g_feedback = L"Launching game client\u2026";
            g_feedbackErr = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);

            std::string err = launch_game();
            if (err.empty()) {
                g_feedback = L"Game client launched successfully.";
                g_feedbackErr = false;
            } else {
                EnableWindow(GetDlgItem(hwnd, ID_LAUNCH), TRUE);
                g_feedback = to_wide(err);
                g_feedbackErr = true;
                MessageBoxA(hwnd, err.c_str(), "Launch Failed", MB_OK | MB_ICONERROR);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
//  WinMain Entry Point
// ===========================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    g_brBg    = CreateSolidBrush(kBg);
    g_brCard  = CreateSolidBrush(kCardBg);
    g_brField = CreateSolidBrush(kFieldBg);

    g_fntTitle  = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
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

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT,
        L"SandalCoreLauncher",
        L"Sandal Core \u2014 Ashes of Creation Emulator",
        style | WS_VISIBLE,
        sx, sy, ww, wh, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    int exitCode = 0;

    // Check for --screenshot <path> argument (e.g. for testing / headless preview)
    int numArgs = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &numArgs);
    if (argvW) {
        for (int i = 1; i < numArgs; ++i) {
            if (std::wstring(argvW[i]) == L"--screenshot" && i + 1 < numArgs) {
                capture_launcher_to_file(hwnd, argvW[i + 1]);
                LocalFree(argvW);
                DestroyWindow(hwnd);
                goto cleanup;
            }
        }
        LocalFree(argvW);
    }

    {
        MSG m{};
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && (m.lParam & (1 << 30)))
                continue;
            if (!IsDialogMessageW(hwnd, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        exitCode = (int)m.wParam;
    }

cleanup:
    DeleteObject(g_fntTitle);
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

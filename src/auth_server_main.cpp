// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  auth_server - tiny local HTTP auth backend for launcher.exe
// ============================================================================
//
// launcher.exe posts JSON to /api/v1/auth/login or /api/v1/auth/register and
// only requires an access_token field to unlock the local launch button. The
// game-facing gRPC auth/xclient services remain in aoc_server.exe.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <random>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>

namespace {

static std::ofstream g_logFile;

std::string now_string() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void log(const std::string& message) {
    const std::string line = "[" + now_string() + "] " + message;
    std::cout << line << std::endl;
    if (g_logFile.is_open()) {
        g_logFile << line << std::endl;
        g_logFile.flush();
    }
}

std::string random_hex(size_t bytes) {
    static std::random_device rd;
    static std::mt19937_64 rng(rd());
    static constexpr char kHex[] = "0123456789abcdef";
// (C.Y.B | CoA)

    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        const auto v = static_cast<unsigned>(rng() & 0xFF);
        out.push_back(kHex[(v >> 4) & 0xF]);
        out.push_back(kHex[v & 0xF]);
    }
    return out;
}

bool send_all(SOCKET sock, const std::string& data) {
    const char* ptr = data.data();
    int remaining = static_cast<int>(data.size());
    while (remaining > 0) {
        const int sent = ::send(sock, ptr, remaining, 0);
        if (sent == SOCKET_ERROR) {
            return false;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

std::string header_value(const std::string& headers, const std::string& name) {
    std::string needle = name + ":";
    auto pos = headers.find(needle);
    if (pos == std::string::npos) {
        for (char& c : needle) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::string lower = headers;
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        pos = lower.find(needle);
        if (pos == std::string::npos) {
            return {};
        }
    }

    pos += needle.size();
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) {
        ++pos;
    }
    auto end = headers.find("\r\n", pos);
    if (end == std::string::npos) {
        end = headers.size();
    }
    return headers.substr(pos, end - pos);
}

std::string http_response(int status, const std::string& reason,
                          const std::string& body,
                          const std::string& content_type = "application/json") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Headers: content-type\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

std::string ok_auth_response(const std::string& username) {
    nlohmann::json j;
    j["access_token"] = "local_" + random_hex(24);
    j["refresh_token"] = "local_refresh_" + random_hex(24);
    j["token_type"] = "Bearer";
    j["expires_in"] = 86400;
    j["username"] = username.empty() ? "local_user" : username;
    return http_response(200, "OK", j.dump());
}

std::string error_response(int status, const std::string& reason,
                           const std::string& message) {
    nlohmann::json j;
    j["error"] = reason;
    j["message"] = message;
    return http_response(status, reason, j.dump());
}

void handle_client(SOCKET client) {
    std::string request;
    request.reserve(8192);

    char buf[4096];
    size_t header_end = std::string::npos;
    size_t expected_size = 0;

    while (request.size() < 1024 * 1024) {
        const int got = ::recv(client, buf, sizeof(buf), 0);
        if (got <= 0) {
            break;
        }
        request.append(buf, got);

        if (header_end == std::string::npos) {
            header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const std::string headers = request.substr(0, header_end + 2);
                const std::string cl = header_value(headers, "Content-Length");
                const size_t body_len = cl.empty() ? 0 : static_cast<size_t>(std::strtoull(cl.c_str(), nullptr, 10));
                expected_size = header_end + 4 + body_len;
            }
        }

        if (expected_size != 0 && request.size() >= expected_size) {
            break;
        }
    }
// C:Y:B and CoA

    std::istringstream first_line(request);
    std::string method;
    std::string path;
    std::string version;
    first_line >> method >> path >> version;

    std::string response;
    if (method == "OPTIONS") {
        response = http_response(204, "No Content", "");
    } else if (method == "GET" && (path == "/health" || path == "/")) {
        response = http_response(200, "OK", R"({"status":"ok"})");
    } else if (method == "POST" &&
               (path == "/api/v1/auth/login" || path == "/api/v1/auth/register")) {
        std::string username;
        const auto body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos) {
            const std::string body = request.substr(body_pos + 4);
            try {
                const auto parsed = nlohmann::json::parse(body);
                username = parsed.value("username", "");
            } catch (...) {
                // Keep local launcher auth permissive; malformed JSON should not
                // block emulator startup.
            }
        }
        log(method + " " + path + " user='" + (username.empty() ? "local_user" : username) + "'");
        response = ok_auth_response(username);
    } else {
        response = error_response(404, "Not Found", "Unknown endpoint.");
    }

    send_all(client, response);
    ::shutdown(client, SD_BOTH);
    ::closesocket(client);
}

} // namespace

int main(int argc, char** argv) {
    uint16_t port = 8081;
    if (argc >= 2) {
        const int parsed = std::atoi(argv[1]);
        if (parsed > 0 && parsed <= 65535) {
            port = static_cast<uint16_t>(parsed);
        }
    }

    std::string log_path = "logs/mock_auth.log";
    if (const char* env_path = std::getenv("AUTH_SERVER_LOG_FILE")) {
        log_path = env_path;
    }
    if (argc >= 3) {
        log_path = argv[2];
    }
    try {
        std::filesystem::path lp(log_path);
        if (lp.has_parent_path()) {
            std::filesystem::create_directories(lp.parent_path());
        }
        g_logFile.open(log_path, std::ios::out | std::ios::app);
    } catch (...) {}

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    SOCKET server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        std::cerr << "socket failed: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }

    BOOL reuse = TRUE;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "bind failed on 127.0.0.1:" << port
                  << " error=" << WSAGetLastError() << std::endl;
        ::closesocket(server);
        WSACleanup();
        return 1;
    }

    if (::listen(server, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen failed: " << WSAGetLastError() << std::endl;
        ::closesocket(server);
        WSACleanup();
        return 1;
    }

    log("auth_server listening on http://127.0.0.1:" + std::to_string(port));

    for (;;) {
        SOCKET client = ::accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            std::cerr << "accept failed: " << WSAGetLastError() << std::endl;
            continue;
        }
        handle_client(client);
    }
}

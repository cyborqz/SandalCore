// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/udp_socket.cpp
// ============================================================================
#include "net/udp_socket.h"
#include <charconv>
#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aoc { namespace worldd {

namespace {

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WinsockInit() { WSACleanup(); }
};
void ensure_winsock() { static WinsockInit init; }
constexpr intptr_t kInvalid = static_cast<intptr_t>(INVALID_SOCKET);
void close_handle(intptr_t h) { closesocket(static_cast<SOCKET>(h)); }
#else
void ensure_winsock() {}
constexpr intptr_t kInvalid = -1;
void close_handle(intptr_t h) { ::close(static_cast<int>(h)); }
#endif

sockaddr_in to_sockaddr(const Address& a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(a.ip);
    sa.sin_port = htons(a.port);
    return sa;
}

Address from_sockaddr(const sockaddr_in& sa) {
    return {ntohl(sa.sin_addr.s_addr), ntohs(sa.sin_port)};
}

}  // namespace

std::string Address::to_string() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                  (ip >> 8) & 0xFF, ip & 0xFF, port);
    return buf;
}

std::optional<Address> parse_address(const std::string& host_port) {
    unsigned parts[5]{};
    const char* next = host_port.data();
    const char* end = next + host_port.size();
    for (size_t i = 0; i < 5; ++i) {
        const auto parsed = std::from_chars(next, end, parts[i]);
        if (parsed.ec != std::errc{} || parts[i] > (i == 4 ? 65535u : 255u)) return std::nullopt;
        if (i == 4) {
            if (parsed.ptr != end) return std::nullopt;
        } else {
            if (parsed.ptr == end || *parsed.ptr != (i == 3 ? ':' : '.')) return std::nullopt;
            next = parsed.ptr + 1;
        }
    }
    return Address{(parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3], static_cast<uint16_t>(parts[4])};
}

std::unique_ptr<UdpSocket> UdpSocket::open(const Address& local, std::string* error) {
    ensure_winsock();
    const intptr_t h = static_cast<intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (h == kInvalid) {
        if (error) *error = "socket() failed";
        return nullptr;
    }
#ifdef _WIN32
    // Without this, an ICMP "port unreachable" from a client that went away
    // makes the next recvfrom fail with WSAECONNRESET.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(static_cast<SOCKET>(h), SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned,
             nullptr, nullptr);
#endif
    const sockaddr_in sa = to_sockaddr(local);
    if (::bind(static_cast<decltype(::socket(0, 0, 0))>(h), reinterpret_cast<const sockaddr*>(&sa),
               sizeof(sa)) != 0) {
        if (error) *error = "cannot bind UDP " + local.to_string() + " (is another server using it?)";
        close_handle(h);
        return nullptr;
    }
    return std::unique_ptr<UdpSocket>(new UdpSocket(h));
}
// Cyb captures, C-O-A code

UdpSocket::~UdpSocket() { close_handle(handle_); }

std::optional<size_t> UdpSocket::receive(uint8_t* buf, size_t cap, Address& from,
                                         std::chrono::milliseconds timeout) {
    const auto s = static_cast<decltype(::socket(0, 0, 0))>(handle_);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int ready = ::select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv);
    if (ready < 0) return std::nullopt;
    if (ready == 0) return size_t{0};
    sockaddr_in sa{};
    socklen_t sa_len = sizeof(sa);
    const auto n = ::recvfrom(s, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0,
                              reinterpret_cast<sockaddr*>(&sa), &sa_len);
    if (n < 0) return size_t{0};  // e.g. a datagram larger than `cap`: dropped
    from = from_sockaddr(sa);
    return static_cast<size_t>(n);
}

bool UdpSocket::send(const Address& to, const uint8_t* data, size_t len) {
    const sockaddr_in sa = to_sockaddr(to);
    const auto s = static_cast<decltype(::socket(0, 0, 0))>(handle_);
    return ::sendto(s, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                    reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == static_cast<int>(len);
}

Address UdpSocket::local_address() const {
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    ::getsockname(static_cast<decltype(::socket(0, 0, 0))>(handle_), reinterpret_cast<sockaddr*>(&sa), &len);
    return from_sockaddr(sa);
}

}} // namespace aoc::worldd

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/service_token.cpp
// ============================================================================
#include "svc/service_token.h"
#include <fstream>
#include <sstream>

namespace aoc { namespace worldd { namespace svc {

std::optional<std::string> load_service_token(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot read " + path + " (scripts\\services.ps1 run creates it)";
        return std::nullopt;
    }
    std::string text(4096, '\0');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(f.gcount()));
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    const std::string token = first == std::string::npos ? "" : text.substr(first, last - first + 1);
    bool hex = token.size() == 64;
    for (char c : token) hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    if (!hex) {
        if (error) *error = path + " does not hold a service token (64 lowercase hex digits)";
        return std::nullopt;
    }
    return token;
}

}}} // namespace aoc::worldd::svc

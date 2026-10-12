// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/service_token.h
//
//  The shared secret the internal services require of each other (plan A4).
//  scripts\services.ps1 writes 32 random bytes as 64 lowercase hex digits to
//  dist\services\service.token (git-ignored); every internal gRPC call
//  carries it as "authorization: Bearer <token>".  Never log it.
// ============================================================================
#pragma once

#include <optional>
#include <string>

namespace aoc { namespace worldd { namespace svc {

constexpr const char* kAuthorizationMetadata = "authorization";

/// Reads the token file: surrounding whitespace is ignored, the rest must be
/// 64 lowercase hex digits.  On failure returns nullopt and says why in
/// `error` (without the file's contents).
std::optional<std::string> load_service_token(const std::string& path, std::string* error);

}}} // namespace aoc::worldd::svc

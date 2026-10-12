// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  services/tether/ics_token_acl.cpp
//
//  The saved login token is a bearer secret for 24 hours, so only the user
//  who signed in may read it.  On Windows the file gets a protected DACL with
//  one entry, for the current user: without it, it inherits from dist\ (on
//  this machine X:\ grants Authenticated Users Modify).  Elsewhere: mode 0600.
//  Kept out of ics_token_file.h so the header does not pull in <windows.h>.
// ============================================================================
#include "services/tether/ics_token_file.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <vector>
#endif

namespace aoc { namespace ics_token {

#ifdef _WIN32

namespace {

// The current user's SID; `buf` holds it.
PSID current_user(std::vector<unsigned char>& buf) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    buf.resize(len);
    const BOOL ok = GetTokenInformation(token, TokenUser, buf.data(), len, &len);
    CloseHandle(token);
    return ok ? reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid : nullptr;
}

}  // namespace

bool restrict_to_owner(const std::string& path) {
    std::vector<unsigned char> buf;
    PSID user = current_user(buf);
    if (!user) return false;
    EXPLICIT_ACCESS_W ea{};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(user);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &ea, nullptr, &acl) != ERROR_SUCCESS) return false;
    std::wstring wide = std::filesystem::path(path).wstring();
    const DWORD rc = SetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                           nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return rc == ERROR_SUCCESS;
}
// ~ C.O.A - Cyb ~

bool is_owner_only(const std::string& path) {
    std::vector<unsigned char> buf;
    PSID user = current_user(buf);
    if (!user) return false;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    std::wstring wide = std::filesystem::path(path).wstring();
    if (GetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                              nullptr, &sd) != ERROR_SUCCESS)
        return false;
    bool only = false;
    ACL_SIZE_INFORMATION info{};
    if (dacl && GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation) && info.AceCount == 1) {
        void* ace = nullptr;
        if (GetAce(dacl, 0, &ace)) {
            auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            only = allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                   EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), user);
        }
    }
    LocalFree(sd);
    return only;
}

#else

bool restrict_to_owner(const std::string& path) {
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return !ec;
}

bool is_owner_only(const std::string& path) {
    using P = std::filesystem::perms;
    std::error_code ec;
    const P p = std::filesystem::status(path, ec).permissions();
    return !ec && (p & (P::group_all | P::others_all)) == P::none;
}

#endif

}} // namespace aoc::ics_token

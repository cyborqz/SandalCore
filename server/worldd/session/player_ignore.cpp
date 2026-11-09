// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_ignore.cpp  (social, step 6: the ignore list)
//
//  The players a character ignores.  The client calls ServerIgnoreCharacter(InCharacterName) and ServerRemoveIgnoreCharacter(InCharacterName) and waits for no answer call: the session keeps the
//  character's list and the pawn's OwnerInfo array IgnoredCharacterNames follows it.  IgnoredGuildNames carries the guild list, which this step only keeps, sends and saves: the calls for a
//  guild (ServerIgnoreGuild, ServerRemoveIgnoreGuild) are a later step.  The game client shows both lists in its own window and hides the chat lines of every name on them by itself, whatever
//  their category, so the server still sends every line and filters none.
//
//  What a call does.
//    ignore    a name that is blank, over 64 bytes, not UTF-8 text or has a control character is refused, and so is the character's own name; a name that is on the list already leaves it as it
//              is; a list of 50 refuses the 51st; any other name goes at the end of the list as it was typed (the spaces at its ends are cut off), whether or not a character of that name exists,
//              since the list holds names.
//    unignore  the name goes off the list; a name that is not on it leaves it as it is.
//  Names are the same without regard to the case of the letters A to Z, as in the client's own list.  A refusal and a call that leaves the list as it is send nothing and are logged (the first
//  few at info, the rest at debug).
//
//  A change reaches the client at once, as the OwnerInfo array, and goes to the saver with the rest of the progress (the character service keeps the lists in character_ignored_names).  The
//  final save of a session that changed its list carries the lists again.  A save the character service never takes is lost, and the saver says so in the log.
// ============================================================================
#include "session/player_session.h"

#include "protocol/repl/codecs.h"
#include "protocol/repl/rpc_fields.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// The longest name on a list, in bytes (the character service refuses a longer one).
constexpr size_t kMaxIgnoredNameBytes = 64;

/// The letters A to Z as lower case and every other character as it is: the game client's list finds a name without regard to the case of letters, and folding more than it does could take two names
/// for one.
char folded(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return folded(x) == folded(y); });
}

std::vector<std::string>::iterator find_name(std::vector<std::string>& list, const std::string& name) {
    return std::find_if(list.begin(), list.end(), [&](const std::string& n) { return same_name(n, name); });
}

/// `raw` without the spaces at its ends.
std::string trimmed(const std::string& raw) {
    const size_t first = raw.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    return raw.substr(first, raw.find_last_not_of(' ') - first + 1);
}

/// Why a name (already trimmed) cannot go on a list, or nullptr.
const char* unusable(const std::string& name) {
    if (name.empty()) return "the name is blank";
    if (name.size() > kMaxIgnoredNameBytes) return "the name is over 64 bytes";
    if (!repl::valid_utf8(name)) return "the name is not UTF-8 text";
    if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) return "the name has a control character";
    return nullptr;
}

/// A player's text as it may go into the log: a control character becomes '?', and a long text is cut.
std::string shown(const std::string& text) {
    std::string out = text.substr(0, 80);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
    }
    if (text.size() > 80) out += "...";
    return out;
}

/// The name a call carries: nullopt when the call does not decode, an empty string when the string is not on the wire (an empty string is the default).
std::optional<std::string> name_in(const repl::ClientRpc& rpc) {
    if (!rpc.call.decoded) return std::nullopt;
    const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "InCharacterName");
    const std::string* text = v ? v->get_if<std::string>() : nullptr;
    return text ? *text : std::string();
}

}  // namespace

void PlayerSession::init_ignored(const svc::Character& character) {
    // what the character saved, minus what the rules refuse (a name the character service would not have stored, a repeat, what is over 50): the lists the session holds are the lists it saves back
    const auto take = [](const std::vector<std::string>& saved, std::vector<std::string>& into) {
        for (const std::string& raw : saved) {
            const std::string kept = trimmed(raw);
            if (unusable(kept) || into.size() >= bootstrap::kMaxIgnoredNames || find_name(into, kept) != into.end()) continue;
            into.push_back(kept);
        }
    };
    take(character.ignored_character_names, ignored_characters_);
    take(character.ignored_guild_names, ignored_guilds_);
    const size_t saved = character.ignored_character_names.size() + character.ignored_guild_names.size();
    const size_t kept = ignored_characters_.size() + ignored_guilds_.size();
    if (kept < saved) {
        spdlog::warn("[ignore] {}: {} of the {} ignored names the character saved are left out (blank, too long, not UTF-8 text, repeated or over {} in a list)", pb_.setup().name, saved - kept, saved,
                     bootstrap::kMaxIgnoredNames);
    }
    if (kept == 0) return;
    // a character with nothing on its lists joins with the pawn's default state (the first name is an update); the others carry the lists in the pawn's first state
    if (!pb_.set_ignored_names(ignored_characters_, ignored_guilds_)) {
        spdlog::warn("[ignore] {}: the pawn does not take the character's ignored names: the client is not told of them", pb_.setup().name);
        return;
    }
    spdlog::info("[ignore] {}: {} ignored player(s) and {} ignored guild(s) on the character's lists, sent with the join", pb_.setup().name, ignored_characters_.size(), ignored_guilds_.size());
}

void PlayerSession::send_ignored(Connection& conn, bool changed) {
    if (pb_.set_ignored_names(ignored_characters_, ignored_guilds_)) {
        queue("pawn state (ignored names)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("ignored names"); }, conn, /*must_build=*/false);
    } else {
        spdlog::warn("[ignore] {}: the pawn does not take the ignored names: the client is not told of the change", name());
    }
    if (changed) {
        progress_changed_ = true;   // (saved with the progress, at once: and once more with the final save of the session)
        hand_over_progress("the ignore list changed");
    }
}

void PlayerSession::on_ignore_character(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    const std::optional<std::string> sent = name_in(rpc);
    if (!sent) {
        ++ignore_refused_;
        if (++ignore_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[ignore] {}: a ServerIgnoreCharacter does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    const std::string target = trimmed(*sent);
    if (const char* why = unusable(target)) {
        ++ignore_refused_;
        spdlog::log(level_for(ignore_refusals_logged_), "[ignore] {}: ignoring '{}' is refused: {}", name(), shown(*sent), why);
        return;
    }
    if (same_name(target, name())) {
        ++ignore_refused_;
        spdlog::log(level_for(ignore_refusals_logged_), "[ignore] {}: ignoring {} is refused: it is the character's own name", name(), shown(target));
        return;
    }
    if (find_name(ignored_characters_, target) != ignored_characters_.end()) {
        ++ignore_unchanged_;
        spdlog::log(level_for(ignore_unchanged_logged_), "[ignore] {}: {} is on the list already: the list stays as it is ({} player(s))", name(), shown(target), ignored_characters_.size());
        if (room_for(1)) send_ignored(conn, /*changed=*/false);   // (the array goes out as it is: nothing differs, so nothing is built)
        return;
    }
    if (ignored_characters_.size() >= bootstrap::kMaxIgnoredNames) {
        ++ignore_refused_;
        spdlog::log(level_for(ignore_refusals_logged_), "[ignore] {}: ignoring {} is refused: the list holds {} players, the most there can be", name(), shown(target), ignored_characters_.size());
        return;
    }
    if (!room_for(1)) {
        ++ignore_refused_;   // (the replies wait for the connection: the list stays as the client has it)
        return;
    }
    ignored_characters_.push_back(target);
    ++ignore_changes_;
    spdlog::log(level_for(ignore_logged_, 20), "[ignore] {}: ignores {} ({} player(s) on the list)", name(), target, ignored_characters_.size());
    send_ignored(conn, /*changed=*/true);
}

void PlayerSession::on_remove_ignore_character(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    const std::optional<std::string> sent = name_in(rpc);
    if (!sent) {
        ++ignore_refused_;
        if (++ignore_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[ignore] {}: a ServerRemoveIgnoreCharacter does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    const std::string target = trimmed(*sent);
    const auto at = find_name(ignored_characters_, target);   // (a blank or odd name is simply not on the list)
    if (at == ignored_characters_.end()) {
        ++ignore_unchanged_;
        spdlog::log(level_for(ignore_unchanged_logged_), "[ignore] {}: {} is not on the list: the list stays as it is ({} player(s))", name(), shown(*sent), ignored_characters_.size());
        if (room_for(1)) send_ignored(conn, /*changed=*/false);
        return;
    }
    if (!room_for(1)) {
        ++ignore_refused_;
        return;
    }
    const std::string removed = *at;
    ignored_characters_.erase(at);
    ++ignore_changes_;
    spdlog::log(level_for(ignore_logged_, 20), "[ignore] {}: no longer ignores {} ({} player(s) on the list)", name(), removed, ignored_characters_.size());
    send_ignored(conn, /*changed=*/true);
}

void PlayerSession::register_ignore_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerIgnoreCharacter", [](PlayerSession& s, const RpcContext& c) { s.on_ignore_character(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRemoveIgnoreCharacter", [](PlayerSession& s, const RpcContext& c) { s.on_remove_ignore_character(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd

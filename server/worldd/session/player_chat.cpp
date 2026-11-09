// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_chat.cpp
//
//  The first two steps of the social track.
//
//  The welcome line: once a session, after the phase-in, the server sends the player one ClientReceiveChatMessages with one
//  SYSBROADCAST line from "Server".  A zone change (the client ready to phase in again) and the rest of the session send
//  nothing; a new session sends it again.  The session flag is welcome_sent_ (session/members/chat.inc).
//
//  The /say echo: a SendChatMessage(FChatData) is answered with one ClientReceiveChatMessages to the sender.  The client's
//  ChatCategory comes back unchanged (its number is logged), From is the character's name, the Quote goes through as it came
//  (item-link markup included) and the decoration and the GM flag stay empty.  An empty or whitespace-only line and a line
//  over 512 characters are refused: nothing is sent and the refusal is logged.  No range, no routing, no rate limit (the
//  later steps).
// ============================================================================
#include "session/player_session.h"

#include "protocol/repl/rpc_fields.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// The longest line a player may send.  Over this it is refused; the line is a bound of ours (the recordings' longest line is shorter).
constexpr size_t kMaxChatLine = 512;

/// True when `text` holds nothing but ASCII whitespace (an empty line is blank too).
bool blank_line(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

}  // namespace

/// The welcome line, once a session and only after the phase-in was built.  Queued behind the phase-in, so it cannot overtake it.
void PlayerSession::send_welcome(Connection& conn) {
    if (welcome_sent_ || !phase_in_done_ || !replies_.empty() || !room_for(1)) return;
    welcome_sent_ = true;
    queue("ClientReceiveChatMessages (welcome)", [](bootstrap::PlayerBootstrap& pb) {
        return pb.chat_line("Welcome to SandalCore.", 124, "Server");
    }, conn);
}

void PlayerSession::run_chat(Connection& conn, Clock::time_point now) {
    (void)now;
    if (phase_in_done_ && !welcome_sent_) send_welcome(conn);
}

/// SendChatMessage(ChatData): one line back to the sender, the text as sent and the category unchanged.
void PlayerSession::on_send_chat_message(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    if (!rpc.call.decoded) {
        ++chat_refused_;
        if (++chat_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[chat] {}: a SendChatMessage does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    const repl::Value* quote = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "ChatData.Quote");
    const repl::Value* category = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "ChatData.ChatCategory");
    const std::string* text = quote ? quote->get_if<std::string>() : nullptr;
    const uint64_t* number = category ? category->get_if<uint64_t>() : nullptr;
    const std::string line = text ? *text : std::string();
    const uint64_t chat_category = number ? *number : 0;
    if (blank_line(line)) {
        ++chat_refused_;
        spdlog::log(level_for(chat_refusals_logged_), "[chat] {}: an empty line is not echoed", name());
        return;
    }
    if (line.size() > kMaxChatLine) {
        ++chat_refused_;
        spdlog::log(level_for(chat_refusals_logged_), "[chat] {}: a line of {} characters is not echoed (the limit is {})", name(), line.size(), kMaxChatLine);
        return;
    }
    if (!room_for(1)) {
        ++chat_refused_;
        return;
    }
    const std::string from = name();
    ++chat_echoes_;
    spdlog::log(level_for(chat_logged_), "[chat] {}: echoing the line ({} characters, category {})", name(), line.size(), chat_category);
    queue("ClientReceiveChatMessages (say)", [line, chat_category, from](bootstrap::PlayerBootstrap& pb) {
        return pb.chat_line(line, static_cast<int>(chat_category), from);
    }, conn);
}

void PlayerSession::register_chat_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "SendChatMessage", [](PlayerSession& s, const RpcContext& c) { s.on_send_chat_message(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_chat.cpp
//
//  The social track's chat: the welcome line, the /say with its radius, and /tell.
//
//  The welcome line: once a session, after the phase-in, the server sends the player one ClientReceiveChatMessages with one
//  SYSBROADCAST line from "Server".  A zone change (the client ready to phase in again) and the rest of the session send
//  nothing; a new session sends it again.  The session flag is welcome_sent_ (session/members/chat.inc).
//
//  The /say: a SendChatMessage(FChatData) with ChatCategory 9 is answered with one ClientReceiveChatMessages to the sender
//  (the step-2 echo, kept) and one to every other player in this world whose pawn is within 30 m on the ground.  The
//  distance is the horizontal one the view's relevancy uses (world/relevancy.h): height does not count.  Every other
//  category keeps the step-2 echo to the sender only.  The client's ChatCategory comes back unchanged (its number is
//  logged), From is the character's name, the Quote goes through as it came (item-link markup included) and the
//  decoration and the GM flag stay empty.  An empty or whitespace-only line and a line over 512 characters are refused:
//  nothing is sent and the refusal is logged.
//
//  The /tell: a SendChatMessage with ChatCategory 45 and a TargetName.  The sender reads "To <TargetName>: <text>" with
//  From empty and the player named reads the text with the sender's name, both category 45 (the recordings' two copies).
//  A tell to oneself sends both copies; a name that is offline or unknown sends the sender's copy and one SYSBROADCAST
//  line "<name> is not online."; names match in any case; a tell has no range; the text is capped at 512 characters like
//  the say.  Nothing else receives it.
//
//  The directory: the sessions of one world find each other through ChatEntry (session/members/chat.inc): the entry is
//  added when the session is built and removed when it is destroyed, and it learns the session's connection at its first
//  tick.  It is only touched on the network thread (the sessions' own ticks and the client's bunches).
// ============================================================================
#include "session/player_session.h"

#include "protocol/repl/rpc_fields.h"
#include "world/relevancy.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// The longest line a player may send.  Over this it is refused; the line is a bound of ours (the recordings' longest line is shorter).
constexpr size_t kMaxChatLine = 512;

/// The /say radius: 30 m on the ground.  The owner's pick: no game record and no recording names a number.
constexpr double kSayRadiusCm = 3000.0;

/// The client's chat categories this file reads: a say (the client's own say command fills it) and a tell.
constexpr uint64_t kSayCategory = 9, kTellCategory = 45;
/// The category of a server line: the welcome line's and the one that says a tell's name is not online.
constexpr int kSysBroadcastCategory = 124;

/// True when `text` holds nothing but ASCII whitespace (an empty line is blank too).
bool blank_line(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

/// True when `a` and `b` are the same text, case does not count (the GM tools' find_player looks a name up the same way).
bool iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

/// A player's text for the log: a control character (a newline would start a log line of the player's making) becomes '?'.
std::string log_text(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
    }
    return out;
}

}  // namespace

// ---- the chat directory (session/members/chat.inc) -------------------------------------------------------------------------

std::vector<PlayerSession::ChatEntry*>& PlayerSession::ChatEntry::registry() {
    static std::vector<ChatEntry*> entries;
    return entries;
}

PlayerSession::ChatEntry::ChatEntry(PlayerSession* self) : self(self), world(self ? self->world_ : nullptr) {
    if (self) registry().push_back(this);
}

PlayerSession::ChatEntry::~ChatEntry() {
    std::vector<ChatEntry*>& entries = registry();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (*it == this) {
            entries.erase(it);
            break;
        }
    }
}

std::vector<PlayerSession::ChatEntry*> PlayerSession::ChatEntry::in_world(const PlayerWorld* world) {
    std::vector<ChatEntry*> out;
    for (ChatEntry* e : registry()) {
        if (e->world == world && e->self && e->connection && e->self->in_world_for_chat()) out.push_back(e);
    }
    return out;
}

PlayerSession::ChatEntry* PlayerSession::ChatEntry::joined(const PlayerWorld* world, const std::array<uint32_t, 4>& guid) {
    for (ChatEntry* e : registry()) {
        // (the connection is not asked for: a session that has not ticked yet still holds the character's lists; only its update waits)
        if (e->world != world || !e->self || e->self->finished_ || e->self->failed_) continue;
        if (e->self->pb_.setup().character_guid == guid) return e;
    }
    return nullptr;
}

PlayerSession::ChatEntry* PlayerSession::ChatEntry::find(const PlayerWorld* world, const std::string& name) {
    if (name.empty()) return nullptr;
    for (ChatEntry* e : registry()) {
        if (e->world != world || !e->self || !e->connection || !e->self->in_world_for_chat()) continue;
        if (iequal(e->self->name(), name)) return e;
    }
    return nullptr;
}

// ---- the welcome line -------------------------------------------------------------------------------------------------------

/// The welcome line, once a session and only after the phase-in was built.  Queued behind the phase-in, so it cannot overtake it.
void PlayerSession::send_welcome(Connection& conn) {
    if (welcome_sent_ || !phase_in_done_ || !replies_.empty() || !room_for(1)) return;
    welcome_sent_ = true;
    queue("ClientReceiveChatMessages (welcome)", [](bootstrap::PlayerBootstrap& pb) {
        return pb.chat_line("Welcome to SandalCore.", kSysBroadcastCategory, "Server");
    }, conn);
}

void PlayerSession::run_chat(Connection& conn, Clock::time_point now) {
    (void)now;
    chat_entry_.connection = &conn;   // the chat directory reaches this session through it
    if (phase_in_done_ && !welcome_sent_) send_welcome(conn);
}

/// One chat line another session sends this player: queued behind what the session already has, or dropped and counted.
void PlayerSession::receive_chat_line(Connection& conn, std::string text, int category, std::string from) {
    if (!room_for(1)) {
        ++chat_dropped_;
        return;
    }
    ++chat_received_;
    queue("ClientReceiveChatMessages (received)", [text = std::move(text), category, from = std::move(from)](bootstrap::PlayerBootstrap& pb) {
        return pb.chat_line(text, category, from);
    }, conn);
}

/// SendChatMessage(ChatData): the step-2 echo, a /say's fan-out, a /tell's two copies, or a refusal.
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
    const repl::Value* target = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "ChatData.TargetName");
    const std::string* text = quote ? quote->get_if<std::string>() : nullptr;
    const uint64_t* number = category ? category->get_if<uint64_t>() : nullptr;
    const std::string* target_text = target ? target->get_if<std::string>() : nullptr;
    const std::string line = text ? *text : std::string();
    const uint64_t chat_category = number ? *number : 0;
    const std::string target_name = target_text ? *target_text : std::string();
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
    if (chat_category == kTellCategory && target_name.empty()) {
        ++chat_refused_;
        spdlog::log(level_for(chat_refusals_logged_), "[chat] {}: a tell with no name is not sent", name());
        return;
    }
    const std::string from = name();
    if (chat_category == kSayCategory) {
        if (!room_for(1)) {
            ++chat_refused_;
            return;
        }
        ++chat_echoes_;
        spdlog::log(level_for(chat_logged_), "[chat] {}: echoing the line ({} characters, category {})", name(), line.size(), chat_category);
        queue("ClientReceiveChatMessages (say)", [line, chat_category, from](bootstrap::PlayerBootstrap& pb) {
            return pb.chat_line(line, static_cast<int>(chat_category), from);
        }, conn);
        // every other player of this world whose pawn is within the radius on the ground
        const GoodPosition here = last_position();
        size_t receivers = 0;
        for (ChatEntry* e : ChatEntry::in_world(world_)) {
            if (e->self == this) continue;
            const GoodPosition there = e->self->last_position();
            if (world::horizontal_distance(here.x, here.y, there.x, there.y) > kSayRadiusCm) continue;
            e->self->receive_chat_line(*e->connection, line, static_cast<int>(chat_category), from);
            ++receivers;
        }
        spdlog::log(level_for(chat_logged_), "[chat] {}: the say reached {} other player(s) within {:.0f} m", name(), receivers, kSayRadiusCm / 100.0);
        return;
    }
    if (chat_category == kTellCategory) {
        if (!room_for(2)) {
            ++chat_refused_;
            return;
        }
        ++chat_echoes_;
        const std::string copy = "To " + target_name + ": " + line;
        spdlog::log(level_for(chat_logged_), "[chat] {}: a tell to \"{}\" ({} characters)", name(), log_text(target_name), line.size());
        queue("ClientReceiveChatMessages (tell)", [copy, chat_category](bootstrap::PlayerBootstrap& pb) {
            return pb.chat_line(copy, static_cast<int>(chat_category), std::string());
        }, conn);
        if (ChatEntry* e = ChatEntry::find(world_, target_name)) {
            e->self->receive_chat_line(*e->connection, line, static_cast<int>(chat_category), from);
        } else {
            const std::string miss = target_name + " is not online.";
            queue("ClientReceiveChatMessages (tell)", [miss](bootstrap::PlayerBootstrap& pb) {
                return pb.chat_line(miss, kSysBroadcastCategory, "Server");
            }, conn);
            spdlog::log(level_for(chat_logged_), "[chat] {}: no player named \"{}\" is online", name(), log_text(target_name));
        }
        return;
    }
    // every other category: the step-2 echo to the sender only
    if (!room_for(1)) {
        ++chat_refused_;
        return;
    }
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

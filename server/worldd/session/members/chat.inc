// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/chat.inc: the members of the chat feature (session/player_chat.cpp)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// The lines the session sent back to its player, and the ones it refused (empty, whitespace-only, over 512 characters or unreadable).
    size_t chat_lines_sent() const { return chat_echoes_; }
    size_t chat_lines_refused() const { return chat_refused_; }
    /// True once the session's welcome line was queued (it is queued once its phase-in was built).
    bool   welcome_line_sent() const { return welcome_sent_; }

private:
    /// Every tick: the welcome line, once the phase-in was built.
    void run_chat(Connection& conn, Clock::time_point now);
    /// Queues the welcome line when it is due and the reply queue has room.
    void send_welcome(Connection& conn);
    /// Server SendChatMessage: one line back to the sender, or a refusal.
    void on_send_chat_message(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    bool   welcome_sent_ = false;
    size_t chat_echoes_ = 0, chat_refused_ = 0, chat_logged_ = 0, chat_refusals_logged_ = 0, chat_unreadable_logged_ = 0;

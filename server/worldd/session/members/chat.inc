// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/chat.inc: the members of the chat feature (session/player_chat.cpp)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// The lines the session sent back to its player, and the ones it refused: empty, whitespace-only, over 512 characters or unreadable, a tell with no
    /// name (or one of spaces alone), and a line the session's reply queue had no room for.
    size_t chat_lines_sent() const { return chat_echoes_; }
    size_t chat_lines_refused() const { return chat_refused_; }
    /// The lines another player's /say or /tell sent this player, and the ones that did not fit the reply queue or were still held when the session ended.
    size_t chat_lines_received() const { return chat_received_; }
    size_t chat_lines_dropped() const { return chat_dropped_; }
    /// True once the session's welcome line was queued (it is queued once its phase-in was built).
    bool   welcome_line_sent() const { return welcome_sent_; }

    /// One session's place in the chat directory of its world: the other sessions a /say in range or a /tell reaches.  The
    /// entry is added when the session is built and removed when it is destroyed, and it learns the session's connection at
    /// its first tick.  The directory is only touched on the network thread (session/player_chat.cpp).
    struct ChatEntry {
        explicit ChatEntry(PlayerSession* self);
        ~ChatEntry();
        ChatEntry(const ChatEntry&) = delete;
        ChatEntry& operator=(const ChatEntry&) = delete;
        PlayerSession*     self = nullptr;
        Connection*        connection = nullptr;   // the session's own connection, set at its first tick
        const PlayerWorld* world = nullptr;        // the world the session is in: a /say and a /tell reach their own world only
        /// The entries of `world` a line can reach, in the order the sessions joined.
        static std::vector<ChatEntry*> in_world(const PlayerWorld* world);
        /// The entry of the player named `name` (case does not count) in `world` that a line can reach, or nullptr.
        static ChatEntry* find(const PlayerWorld* world, const std::string& name);
        /// The entry of the session with this character GUID in `world` that has joined (in the world or still connecting), or nullptr: a friend call's
        /// other side is that session before it phases in too, so a request for it is not saved beside a session that will save over it.
        static ChatEntry* joined(const PlayerWorld* world, const std::array<uint32_t, 4>& guid);
        /// The entry of the player named `name` (case does not count) in `world` who has joined and has not phased in yet, or nullptr: a tell to a
        /// player who is still loading is held by that session until it is in the world.  A known connection that is going away is no such player.
        static ChatEntry* loading(const PlayerWorld* world, const std::string& name);
    private:
        /// True when a line can reach the player: he is in the world, his connection is known, and it is not going away (the server queued its close,
        /// as a kick does, the client closed it, or it broke the protocol).
        bool reachable() const;
        static std::vector<ChatEntry*>& registry();
    };

private:
    /// Every tick: the welcome line, once the phase-in was built, and the session's connection for the chat directory.
    void run_chat(Connection& conn, Clock::time_point now);
    /// Queues the welcome line when it is due and the reply queue has room.
    void send_welcome(Connection& conn);
    /// Server SendChatMessage: the step-2 echo, a /say's fan-out, a /tell's two copies, or a refusal.
    void on_send_chat_message(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// One chat line another session sends this player (a /say in range, a /tell): queued behind what the session already has (true), or dropped and counted when
    /// the queue has no room (false).
    bool receive_chat_line(Connection& conn, std::string text, int category, std::string from);
    /// True when the player is in the world: its pawn is there after the phase-in, and a finished or failed session is not.
    bool in_world_for_chat() const { return phase_in_done_ && !finished_ && !failed_; }
    /// A tell held for this session while it is still loading: the text, the client's category and the sender's name, in the order sent.
    struct HeldTell {
        std::string text;
        int         category = 0;
        std::string from;
    };
    /// Holds a tell for the player until the phase-in is built and the welcome line is queued; false, with the line counted as dropped, when the
    /// session already holds kMaxPendingReplies of them.
    bool hold_chat_line(std::string text, int category, std::string from);
    /// Delivers the held tells in the order sent, each through receive_chat_line, as many as the reply queue has room for in this tick.
    void deliver_held_chat_lines(Connection& conn);
    /// Drops the held tells and counts them when the session ends before its phase-in.
    void drop_held_chat_lines();
    bool   welcome_sent_ = false;
    size_t chat_echoes_ = 0, chat_refused_ = 0, chat_logged_ = 0, chat_refusals_logged_ = 0, chat_unreadable_logged_ = 0;
    size_t chat_received_ = 0, chat_dropped_ = 0;
    std::deque<HeldTell> held_tells_;
    ChatEntry chat_entry_{this};

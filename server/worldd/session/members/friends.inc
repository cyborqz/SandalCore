// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  session/members/friends.inc: the members of the friends list (social, step 7; session/player_friends.inc)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// The friends and the pending friend requests the character has, as the session keeps them: each list in the order the character service sent it (oldest first).
    const std::vector<svc::Friend>& friends() const { return friends_; }
    const std::vector<svc::FriendRequest>& friend_requests() const { return friend_requests_; }

private:
    /// One side of a friend call: this session or the character's own session in this world (then `session`), or the character service's record when it has no
    /// session here (then the lists are saved whole).  Load, change, store.
    struct FriendSide {
        PlayerSession*                  session = nullptr;
        std::string                     id, name;
        int32_t                         level = 0, class_id = 0;
        std::vector<svc::Friend>        friends;
        std::vector<svc::FriendRequest> requests;
    };
    /// The join: the friends and the pending requests the character saved.  A request at or past its expiry is left out, and a list over its cap is cut, so that the session
    /// saves back only what the rules allow.  The lists go to the SocialComponent's first block (the online flags as the sessions of this world are now) and the session's
    /// friend GUIDs into the reverse index.
    void init_friends(const svc::Character& character);
    /// The session of the player with this character GUID in this world, or nullptr (the chat directory's rule: in the world now).
    PlayerSession* friend_session_of(const std::array<uint32_t, 4>& guid) const;
    /// True when a session of this world has the character now: what the online flag means.
    bool friend_online(const std::array<uint32_t, 4>& guid) const;
    /// The two lists as the SocialComponent carries them (the online flag from this world, the expiry in Unix seconds); sets them on the bootstrap.
    void refresh_friend_wire();
    /// Sends the SocialComponent's change, if any (nothing when the arrays are as the connection has them).
    void send_friend_update(Connection& conn);
    /// The reverse index (friend GUID -> the sessions whose FriendsList names it) loses this session's old keys and gains its current friends'.  Called whenever the
    /// list changes and once at the end of the session.
    void friends_index_refresh();
    /// Tells every session of this world whose FriendsList names this character that its online state changed; each refreshes its wire lists and is sent the change.
    void announce_friends_online(bool online);
    /// Every tick: the first after the phase-in announces this session as online and brings its own list's online flags up to date (a friend who phased in between the
    /// join's first block and now is a one-item change, not a new first block).
    void run_friends(Connection& conn, Clock::time_point now);
    /// The session is over: the sessions that list this character are told it is offline.
    void leave_friends(const char* why);
    /// The destructor: take this session out of the reverse index (the lists go with it).
    void forget_friends();
    /// Queues the answers of the name checks the lookup worker has finished; called every tick (whatever the phase-in).
    void drain_friend_checks(Connection& conn);
    /// The two sides of a call, loaded and stored (a session's copy for a player of this world, the character service's record for an offline one).
    bool friend_load_self(FriendSide& side);
    bool friend_load_other(const std::array<uint32_t, 4>& guid, FriendSide& side);
    bool friend_store(FriendSide& side, Connection* self_conn, const char* why);
    /// ServerSendFriendRequest(RecipientCharacterName), ServerAcceptFriendRequest(SenderCharacterName), ServerDeclineFriendRequest(SenderCharacterName),
    /// ServerRemoveFriend(FriendToRemoveGuid) and ServerCheckIfCharacterNameExistsRequest(CharacterNameToCheck).
    void on_send_friend_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_accept_friend_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_decline_friend_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_remove_friend(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_check_character_name(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    std::vector<svc::Friend>        friends_;
    std::vector<svc::FriendRequest> friend_requests_;
    /// The friend GUIDs this session is registered under in the reverse index.
    std::vector<std::array<uint32_t, 4>> friend_index_keys_;
    bool   friends_online_announced_ = false;   // the phase-in told the sessions that list this character (once per session)
    size_t friend_logged_ = 0, friend_refused_logged_ = 0, friend_unreadable_logged_ = 0;
    size_t friend_lines_over_ = 0;   // unknown-name lines queued while the reply queue was full (the first is logged)
    /// The name checks the lookup worker is doing, oldest first: its ticket and the name the client sent.  Drained every tick by drain_friend_checks.
    std::deque<std::pair<uint64_t, std::string>> friend_checks_;
    size_t friend_checks_dropped_ = 0;   // the checks over the 64 that may wait at once (the first drop is logged)

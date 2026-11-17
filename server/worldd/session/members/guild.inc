// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  session/members/guild.inc: the members of the guilds (session/player_guild.inc, social step 9)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// One rank of a guild (the client's FGuildRankData): its name and the 35 permissions as a mask, bit 0 the first of the record's order.
    struct GuildRankRow {
        std::string name;
        uint64_t    permissions = 0;
    };
    /// One member of a guild (the client's FGuildMemberData): the character's GUID and name, its rank, level and class, when it joined and was last seen
    /// online (Unix milliseconds), its group and the guild's GUID.
    struct GuildMemberRow {
        std::array<uint32_t, 4> guid{};
        std::string             name;
        uint8_t                 rank = 0;
        uint8_t                 level = 1;
        uint8_t                 class_id = 0;
        uint64_t                last_online = 0;
        uint64_t                joined = 0;
        int8_t                  group = -1;
        std::array<uint32_t, 4> guild_guid{};
    };
    /// One invitation a guild has out: the invited character, who asked and when it ends (60 s after it was made, the data's InviteDurationSeconds).
    struct GuildInvite {
        std::string             name;
        std::array<uint32_t, 4> guid{};
        std::string             inviter;
        Clock::time_point       expires{};
    };
    /// One guild of this process.  The sessions of this world share it (guild_book()), so a guild change is one state and every online member is told it from
    /// there; it is written whole to the character service after every change (SaveGuild: a create when version is 0, and the version it writes over after that).
    /// The roster is in join order (oldest first).  A guild has one invitation out for each character it invited and that has not answered, each for 60 s.
    struct Guild {
        std::array<uint32_t, 4>       id{};
        std::string                   name;
        int64_t                       version = 0;
        std::vector<GuildRankRow>     ranks;
        int                           level = 1;
        int                           experience = 0;
        int                           capacity = 50;
        int                           level_cap = 10;
        std::vector<GuildMemberRow>   members;
        std::vector<GuildInvite>      invites;
    };

    /// The name of the guild the session is in, empty when it is in none (tests).
    const std::string& guild_name() const { return guild_name_; }
    /// The session's rank in its guild (0 leader, 1 officer, 2 member; -1 when it is in none).
    int guild_rank() const { return guild_rank_; }
    /// The invite the session has waiting (the inviter's name and the guild's), or empty names (tests).
    const std::string& guild_pending_name() const { return guild_pending_name_; }
    const std::string& guild_pending_inviter() const { return guild_pending_inviter_; }
    /// The GM command's answer: what it achieved, or why it refused.
    struct GmGuildResult {
        bool        ok = false;
        std::string refusal;
        std::string text;
    };
    /// The GM command's join/leave (the bot of the load test cannot accept an invitation): puts the session into a guild at a rank, or takes it out.
    GmGuildResult gm_guild(Connection& conn, const std::string& action, const std::string& guild_name, const std::string& rank, Clock::time_point now, const std::string& by);

private:
    /// The guild's join at the login (session/player_guild.inc): the guild the character service sent, its rank and cooldown, or none; the pawn's guild
    /// component and the GuildMember alignment are set from it.
    void init_guild(const svc::Character& character);
    /// The pawn's guild component, in a chain of its own once the character is in the world (the join's chains are the retail login's and hold none): its export, the
    /// guild's state for a member, the GuildMember alignment.  A character in no guild gets the component with nothing in it, so that its client can call the guild.
    void open_guild(Connection& conn);
    /// A guild made here is written to the character service: at once when the sink can ask (the name may be one that a guild none of whose members is online here
    /// has), else queued.  False: the service refused it (the name is taken) and the guild is not made.
    bool guild_register(Guild& g);
    /// Every tick: the invitation that is 60 s old goes, and the roster's levels and online flags are kept current.
    void run_guild(Connection& conn, Clock::time_point now);
    /// A session that ends leaves the roster (its online flag and last online time) without leaving the guild; the other online members are told.
    void leave_guild_on_finish();
    /// A destroyed session is taken out of the roster the same way, without a connection.
    void forget_guild();
    void on_guild_create(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_guild_invite(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_guild_accept(Connection& conn, Clock::time_point now);
    void on_guild_decline(Connection& conn, Clock::time_point now);
    void on_guild_leave(Connection& conn, Clock::time_point now);
    void on_guild_kick(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_guild_disband(Connection& conn, Clock::time_point now);
    void on_guild_reassign(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_guild_assign_leader(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_guild_get_data(Connection& conn, Clock::time_point now);
    void on_guild_update_hierarchy(Connection& conn, Clock::time_point now);
    void on_guild_message(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);

    /// The request's answer on the guild component, with the client's request type and the game's text.
    void guild_reply(Connection& conn, bool ok, uint64_t request_type, const std::string& message);
    /// Writes the guild's state to the session's pawn's guild component and sends the block (one update).
    bool guild_refresh_pawn(Connection& conn);
    /// Every online member of `g` gets the guild's state again (one update each), and the pawn the other players are shown gets the name and alignment.
    void guild_refresh_members(Guild& g, const char* why);
    /// The character's own member row of `g`, or nullptr.
    GuildMemberRow* guild_member_of(Guild& g, const std::array<uint32_t, 4>& guid) const;
    /// The value the pawn's guild component is given: the roster with the online flags from this world's sessions and the live levels.
    bootstrap::GuildWire guild_wire_of(const Guild& g, const GuildMemberRow& self) const;
    /// Writes the guild whole to the character service (a create when version is 0) and makes the version the next save writes over one more.
    void guild_save(Guild& g, bool create);
    /// Writes the guild's disband to the character service.
    void guild_save_disbanded(Guild& g);
    /// Writes the character's join cooldown to the character service.
    void guild_save_cooldown(const std::array<uint32_t, 4>& guid, int64_t until_ms);
    /// `who` joins `g` at `rank`: the roster, the pawn's component, the alignment and every online member's update.
    void guild_add_member(Guild& g, PlayerSession& who, int rank, int64_t now_ms);
    /// `guid` leaves `g`: its cooldown, the leader's leaving (the data's PromoteNextHighestRankingPlayer) and the roster's update; `tell` clears the
    /// leaving player's own component and alignment.  A guild whose last member leaves is erased.
    void guild_remove_member(Guild& g, const std::array<uint32_t, 4>& guid, bool tell, const char* why);
    /// The leader's leaving promotes the member of the highest rank below the leader that joined first (the data's RemovedLeaderAction).
    void guild_promote_next(Guild& g);
    /// The invitation this session waits on is over (it answered, it ran out, or the session ends): the guild forgets it as well.
    void guild_cancel_pending();
    /// The permission of the session's rank (a bit of the client's FGuildPermissionsSettings order).
    bool guild_may(const Guild& g, uint64_t bit) const;
    /// The data's three ranks' names and masks (Guild_Information_Base.DefaultRanks).
    static const std::vector<GuildRankRow>& guild_default_ranks();
    /// A new guild of `name` with its founder as the only member, at the leader's rank (not in the book yet).
    static std::unique_ptr<Guild> guild_found(const std::string& name, const PlayerSession& founder, int64_t now_ms);
    /// The pawn the other players are shown takes the guild's name, when it changed.
    void sync_shown_guild();

    Guild*         guild_ = nullptr;                       // the guild of this world the session is in (nullptr: none)
    int            guild_rank_ = -1;                       // its rank in it
    std::string    guild_name_;                            // the guild's name (empty: none), as the pawn's component and the shown pawn carry it
    std::string    shown_guild_name_;                      // ... and what the pawn the others are shown says now
    int64_t        guild_cooldown_until_ = 0;              // Unix milliseconds; 0: none
    int            guild_level_seen_ = 0;                  // the level the roster last carried for this member
    std::string    guild_pending_name_, guild_pending_inviter_;   // the invitation this session has waiting
    Clock::time_point guild_pending_expires_{};
    size_t         guild_logged_ = 0, guild_refused_logged_ = 0, guild_unreadable_logged_ = 0;

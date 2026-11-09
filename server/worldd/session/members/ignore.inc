// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/ignore.inc: the members of PlayerSession's ignore list (session/player_ignore.cpp)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// The players and the guilds the character ignores, as the session keeps them: the names as typed, each list in the order they were added.
    const std::vector<std::string>& ignored_character_names() const { return ignored_characters_; }
    const std::vector<std::string>& ignored_guild_names() const { return ignored_guilds_; }
    /// The calls that changed the list of players, the ones that were refused (a name that is blank, over 64 bytes, not UTF-8 text, with a control character, the character's own, or a 51st) and the ones
    /// that left it as it was (a name that was on it already, a removal of one that was not).
    size_t ignore_changes() const { return ignore_changes_; }
    size_t ignore_refused() const { return ignore_refused_; }
    size_t ignore_unchanged() const { return ignore_unchanged_; }

private:
    /// The join: the lists the character saved, cleaned of what the rules refuse, go on the pawn with its first state.
    void init_ignored(const svc::Character& character);
    /// ServerIgnoreCharacter(InCharacterName) and ServerRemoveIgnoreCharacter(InCharacterName).
    void on_ignore_character(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_remove_ignore_character(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// Puts the lists on the pawn and queues the pawn's update (it carries nothing when the client holds these lists already).  After a change it hands the progress to the saver as well.
    void send_ignored(Connection& conn, bool changed);
    std::vector<std::string> ignored_characters_, ignored_guilds_;
    size_t ignore_changes_ = 0, ignore_refused_ = 0, ignore_unchanged_ = 0;
    size_t ignore_logged_ = 0, ignore_refusals_logged_ = 0, ignore_unchanged_logged_ = 0, ignore_unreadable_logged_ = 0;

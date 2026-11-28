// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/quest_events.inc: what the character does that a quest counts (the code is in quest/session_quests2.cpp)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
//
//    an ability the server started   UseAbility objectives (Sprint, Dodge, Guard, a weapon combo, a ration or a drink)
//    an item put on                  Collect objectives of the kind Equip
//    items sold to a shop            Collect objectives of the kind Sell
//    a creature killed               Kill objectives, by instance, by set and by tag query (the creature's tags are the population's: its instance, its asset sets and its asset)
// ============================================================================
public:
    /// Tests: the character is in a fight (or out of it) as the world's combat timer would say; the abilities that count only in a fight read it.
    void set_fight_for_tests(bool in_fight);
    /// Tests: the combat world gave the character the kill `kill` (the way the tick applies one: the experience, then the quests).
    void kill_for_tests(Connection& conn, const combat::KillReward& kill, Clock::time_point now);
    /// Tests: the reward tables the quest effects asked to give (a GrantTable action and the quest items a kill drops; a finished quest's own rewards are not in it), in order, the first 64.
    const std::vector<int64_t>& quest_grants() const { return quest_grants_; }

private:
    std::vector<int64_t> quest_grants_;   // (see quest_grants())
    /// The server started the ability `ability` for the character (a request the abilities refused is not one).
    void quest_ability(Connection& conn, int64_t ability, Clock::time_point now);
    /// The character got `count` of the item `item` the way `how` says (Equip, Sell): the Collect objectives of that kind count them.
    void quest_collected(Connection& conn, const char* how, int64_t item, int64_t count, Clock::time_point now);
    /// The gameplay tags of the creature a kill names, in lower case with all their parents (always "character": every creature is one).
    std::set<std::string> creature_tags(const combat::KillReward& kill) const;

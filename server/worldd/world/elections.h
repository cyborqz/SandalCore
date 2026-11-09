// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/elections.h  (node citizenship, stage 2: mayors)
//
//  The mayoral elections of the nodes that take citizens, as one store for the whole world (every session reads it; PlayerWorld owns it and keeps it in a file beside
//  the ground crates, the supervisor's run folder, so an update does not lose it).
//
//    the cycle   EMayorElectionState, from content.db's election_def (CityNodeElectionDefRecordBase Scientific_Election_Definition):
//                  AwaitingFirstElection   from when the node takes citizens until its first citizen;
//                  the first election      RegistrationPeriod (the client's "prevote") FirstTimeRegistrationPeriodDurationSeconds (1 day), ActiveElection
//                                          FirstTimeElectionPeriodDurationSeconds (1 day), AwaitingElectionResults FirstTimeWaitForResultsPeriodDurationSeconds (10 s);
//                  then every month        ActiveElection from day ElectionPeriodStartDay of the month at ElectionPeriodStartHour (the 1st, 00:00 UTC) for
//                                          ElectionPeriodDurationDays (5), through the end of ElectionPeriodEndHour of its last day (hour 23: the vote ends at 00:00
//                                          after the 5th); RegistrationPeriod the RegistrationPeriodDurationDays (5) before it; AwaitingElectionResults one second, the
//                                          results, the post-election state and the mayor at 00:00:01;
//                  PostElection            the mayor's term, until the next month's registration (at least MinimumLengthOfTermDays, 0 in the data).
//                A first registration with no candidate runs again; a monthly one with no candidate skips that month's vote, and the mayor stays.  The day of
//                ElectionPeriodStartDay is read as the day of the month (the data's other days are weekdays by name), the times as UTC.
//    who         the node's citizens (kept here from the sessions' citizenship changes: an account that joins, renounces or lapses), one vote per account; a candidate is a
//                citizen who registered in the registration period.  The mayor's account loses the office with its citizenship.
//    the count   ranked choice (ServerRankedChoiceVoteForMayor sends the ranking; the window sends one): an instant runoff.  Each ballot counts for its first choice
//                still standing (one who withdrew or was counted out passes the ballot on; a ballot with none left counts no more); a majority of the ballots
//                still counting wins, else the candidate with the fewest goes, the later registered of a tie first.  A ballot keeps at most
//                RankedChoiceVoteCountLimit (3) candidates.
//    the dues    the node's dues dates (CitizenshipRules): one schedule for all its citizens, from dues_anchor_ms (its first citizen's join) every period.
//    the rank    a citizen's CitizenshipRank is its place in the node's citizens by when it joined (1: the first), which the dues' rank impact reads.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::world {

enum class ElectionState : uint8_t { AwaitingFirst = 0, Registration = 1, Active = 2, AwaitingResults = 3, Post = 4 };
const char* election_state_name(ElectionState s);

/// A rate the node's mayor set for a NodeTax tag (ServerSetTaxOverride): the percent of the price and the flat amount.
struct TaxOverride {
    float   percent = 0;
    int32_t flat = 0;
    bool operator==(const TaxOverride&) const = default;
};

/// One entry of a node's notification feed (the citizenship window's FNodeNotificationDetails): what happened, when, and what it names.  `type` is the client's
/// ENodeSystemNotificationType (10 Taxes, 12 NewMayoralCommission, 1 to 5 the service buildings ...).
struct NodeNews {
    uint8_t     type = 0;
    int64_t     at_ms = 0;
    std::string mayor;                 // MayorName
    int64_t     plot = 0, building = 0, commission = 0;
    uint8_t     building_state = 0;    // EBuildingState
    bool operator==(const NodeNews&) const = default;
};

struct ElectionRules {
    int64_t first_registration_secs = 86400;   // FirstTimeRegistrationPeriodDurationSeconds
    int64_t first_election_secs = 86400;       // FirstTimeElectionPeriodDurationSeconds
    int64_t results_secs = 10;                 // FirstTimeWaitForResultsPeriodDurationSeconds
    int32_t registration_days = 5;             // RegistrationPeriodDurationDays
    int32_t election_days = 5;                 // ElectionPeriodDurationDays
    int     start_day = 1;                     // ElectionPeriodStartDay: the day of the month
    int     start_hour = 0;                    // ElectionPeriodStartHour (UTC)
    int     end_hour = 23;                     // ElectionPeriodEndHour, of the election's last day: the vote runs through the end of that hour
    int32_t min_term_days = 0;                 // MinimumLengthOfTermDays
    size_t  ranked_limit = 3;                  // ElectionConstants' RankedChoiceVoteCountLimit
    std::map<int64_t, int> min_citizens;       // CityNodeRecordBase's MayorMinimumCitizens by node (not listed, or 0: 1)
    /// The citizens `node` needs to hold an election (at least 1).
    int citizens_for_election(int64_t node) const {
        const auto it = min_citizens.find(node);
        return it == min_citizens.end() || it->second < 1 ? 1 : it->second;
    }
    uint8_t election_type = 0;                 // EMayorElectionType: Scientific 0
    /// The monthly cycle's ActiveElection start: the first one (day start_day of a month at start_hour) whose registration starts at or after `not_before_ms`.
    int64_t election_start_ms(int64_t not_before_ms) const;
    /// The end of the election that starts at `start_ms`: the end of hour end_hour of its election_days-th day.
    int64_t election_end_ms(int64_t start_ms) const;
    /// The results of an election that ended at `end_ms`: one second after it.
    int64_t results_ms(int64_t end_ms) const { return end_ms + 1000; }
    /// content.db's election_def: the first definition that is no test one.  nullopt with `error` for a file without the table.
    static std::optional<ElectionRules> load(const std::string& content_db_path, std::string& error);
};

struct ElectionPerson {
    int64_t     account = 0;
    std::string character_id;   // 32 lower-case hex digits
    std::string name;
    int64_t     at_ms = 0;      // when they joined, registered or voted
    bool operator==(const ElectionPerson&) const = default;
};

struct NodeElection {
    ElectionState state = ElectionState::AwaitingFirst;
    int64_t       phase_started_ms = 0, phase_ends_ms = 0;
    std::vector<ElectionPerson> candidates;                        // in the order they registered
    std::map<int64_t, std::vector<std::string>> votes;             // account -> the ranked candidates' character ids
    std::map<int64_t, ElectionPerson> voters;                      // account -> who voted (the character that voted)
    std::map<int64_t, ElectionPerson> citizens;                    // account -> its primary character
    ElectionPerson mayor;                                          // account 0: no mayor
    std::string    mayor_message;
    bool           first = false;                                  // this cycle is the node's first election (its own lengths)
    bool           dues_disabled = false;                          // a game master switched the node's citizenship dues off (ServerGMDisableCitizenshipDues)
    std::map<std::string, TaxOverride> taxes;                      // the mayor's rates by NodeTax tag (the others are the data's defaults)
    int64_t        treasury = 0;                                   // the node's money (copper): the taxes its shops took
    std::vector<NodeNews> news;                                    // the node's notification feed, oldest first (at most kMaxNews)
    int64_t        dues_anchor_ms = 0;                             // the node's dues schedule: its dates are this plus whole periods (0: none yet)
    bool operator==(const NodeElection&) const = default;
};

class Elections {
public:
    explicit Elections(ElectionRules rules = {}) : rules_(rules) {}
    const ElectionRules& rules() const { return rules_; }

    /// What a phase change did, for the sessions: the node, the new state and, at the results, the winner (empty: no mayor).
    struct Change {
        int64_t       node = 0;
        ElectionState state = ElectionState::AwaitingFirst;
        bool          results = false;
        std::string   winner;
    };
    /// Brings every node to `now_ms`: a node that `takes_citizens` and has no election yet starts AwaitingFirst; each phase whose time ran out gives way to the next
    /// (several, after a long stop).  The changes in order.
    std::vector<Change> advance(int64_t now_ms, const std::function<bool(int64_t node)>& takes_citizens, const std::vector<int64_t>& nodes);
    /// The next phase now, whatever time is left (a game master's).
    std::optional<Change> skip(int64_t node, int64_t now_ms);
    /// The node's election enters `s` now and stays `duration_secs` (0: the phase's own length): a game master's (ServerGMSetElectionStateAndDuration).  A node with no
    /// election yet gets one.  Entering Post counts the votes, as the phase's end does.
    Change set_state(int64_t node, ElectionState s, int64_t now_ms, int64_t duration_secs);

    const NodeElection* node(int64_t guid) const;
    const std::map<int64_t, NodeElection>& all() const { return nodes_; }

    /// The account is a citizen of `node` (its primary character); of no other node (it leaves the citizens, candidates and votes of every other, and the office).
    void set_citizen(int64_t node, const ElectionPerson& who);
    /// The account is a citizen of no node.
    void remove_citizen(int64_t account);

    /// A citizen stands for mayor (the registration period).  False with `why`.
    bool register_candidate(int64_t node, const ElectionPerson& who, int64_t now_ms, std::string& why);
    /// A citizen's ranked vote (the election): `ranked` are candidates' character ids, best first (unknown ones and repeats are dropped; empty: refused).
    bool vote(int64_t node, const ElectionPerson& who, const std::vector<std::string>& ranked, int64_t now_ms, std::string& why);
    /// The mayor's message (at most 512 bytes).
    bool set_message(int64_t node, int64_t account, const std::string& message, std::string& why);
    /// The node's mayor is `who` now (a game master's); account 0 removes the mayor.
    void set_mayor(int64_t node, const ElectionPerson& who, int64_t now_ms);

    // ---- a game master's (the client's console calls; the session checks the game master)
    /// `who` stands (adding) or no longer stands for mayor of `node`, in any phase (ServerGMSetMayorCandidate).  False with `why` for no such election or a
    /// non-citizen to add.
    bool gm_set_candidate(int64_t node, const ElectionPerson& who, bool adding, int64_t now_ms, std::string& why);
    /// The node's mayor's message, set whoever the mayor is (ServerGMSetMayorMessage).  False with `why` for a node with no mayor or a message over 512 bytes.
    bool gm_set_message(int64_t node, const std::string& message, std::string& why);
    /// A vote by `who`, citizen of the node or not, while the node votes (ServerGMRankedChoiceVoteForMayor).
    bool gm_vote(int64_t node, const ElectionPerson& who, const std::vector<std::string>& ranked, int64_t now_ms, std::string& why);
    /// The node's citizen named `name` (any case); null for none.
    const ElectionPerson* citizen_named(int64_t node, const std::string& name) const;
    /// The node's candidate named `name` (any case); null for none.
    const ElectionPerson* candidate_named(int64_t node, const std::string& name) const;
    /// The node's citizenship dues off (ServerGMDisableCitizenshipDues) or on again (ServerGMTryEnableCitizenshipDues).
    void set_dues_disabled(int64_t node, bool off);
    bool dues_disabled(int64_t node) const;

    // ---- the node's taxes and treasury
    /// The node's rate for `tag` is `rate` now.
    void set_tax(int64_t node, const std::string& tag, TaxOverride rate);
    /// The rates the node's mayor set (null: none).
    const std::map<std::string, TaxOverride>* taxes(int64_t node) const;
    /// The node's treasury gains `copper` (a shop's tax; a game master's ServerGMAddNodeGold).
    void add_treasury(int64_t node, int64_t copper);
    int64_t treasury(int64_t node) const;
    /// The node's dues schedule (0: none yet), and its start if it has none (the first citizen's join).
    int64_t dues_anchor(int64_t node) const;
    void    set_dues_anchor(int64_t node, int64_t at_ms);
    /// A notification for the node's feed (the oldest goes past kMaxNews).
    void add_news(int64_t node, NodeNews n);
    static constexpr size_t kMaxNews = 32;

    bool is_candidate(int64_t node, int64_t account) const;
    /// The account's CitizenshipRank in `node`: its place among the node's citizens by when they joined (1: the first; ties by account), 0 when it is no citizen there.
    int citizen_rank(int64_t node, int64_t account) const;
    /// How many citizens `node` has.
    size_t citizen_count(int64_t node) const;
    /// The node whose mayor the account is (0: none).
    int64_t mayor_node(int64_t account) const;

    /// The instant runoff over `e`'s votes: the winner's index in e.candidates, nullopt for no candidate.
    static std::optional<size_t> tally(const NodeElection& e);

    /// Bumped by every change: readers that copied something compare it.
    uint64_t version() const { return version_; }

    std::string to_json() const;
    /// Replaces the store with `json`'s (what to_json wrote).  False with `error`, nothing changed.
    bool from_json(const std::string& json, std::string& error);

private:
    /// `e` enters `s` at `start_ms`.  `count`: entering Post counts the votes (false: a monthly registration had no candidate, the mayor stays).
    void enter(int64_t node, NodeElection& e, ElectionState s, int64_t start_ms, std::vector<Change>& out, bool count = true);
    bool vote_as(int64_t node, const ElectionPerson& who, const std::vector<std::string>& ranked, int64_t now_ms, std::string& why, bool citizens_only);
    ElectionRules                   rules_;
    std::map<int64_t, NodeElection> nodes_;
    uint64_t                        version_ = 0;
};

}  // namespace aoc::worldd::world

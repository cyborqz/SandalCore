// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/citizenship.inc: the members of PlayerSession for node citizenship (session/player_citizenship.cpp, world/citizenship.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// What the Node Administrator offered last: the join dialog or the renounce warning, for that NPC's node.  The application must name that node.
    struct CitizenshipOffer {
        uint64_t          npc = 0;      // the NPC (actor id)
        int64_t           node = 0;     // its node
        int64_t           vendor = 0;   // the shop of the tax receipts the dialog's purchase button opens
        int32_t           cost = 0;     // the Tax Receipts the dialog asked for
        bool              join = true;  // the join dialog (false: the renounce warning)
        Clock::time_point at{};
    };
    /// How long the offer stands (modelled: the client keeps its dialog open as long as the player likes; the server takes an answer within this).
    static constexpr std::chrono::minutes kCitizenshipOfferFor{10};
    /// The Node Administrator's line (dialogue AddNodeCitizenship or RemoveNodeCitizenship): ClientShowAddNodeCitizenshipDialog or ClientShowCitizenshipRenouncementWarning,
    /// or ClientCitizenshipApplicationRejection when the player cannot join that node.
    void open_citizenship_dialog(Connection& conn, const world::WorldActor& npc, bool join, int64_t vendor, Clock::time_point now);
    /// The NPC of the standing join offer whose shop is `vendor` and who is within reach (the dialog's purchase button, ServerOpenVendor, after the conversation ended); null for none.
    const world::WorldActor* citizenship_offer_npc(int64_t vendor, Clock::time_point now) const;
    void on_citizenship_application(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_citizenship_renounce(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_citizenship_dues(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ClientPopulateCitizenshipData: the window's data for the account's citizenship now.
    void send_citizenship_window(Connection& conn);
    /// ServerSetEssentialCitizenshipWindowData: counted (the RPC table's entry answers it, with the connection).
    void on_citizenship_window_request();
    size_t citizenship_requests_ = 0;
    repl::CitizenshipWindowData citizenship_window(int64_t now_ms) const;
    /// The account's citizenship becomes `c`: the world's copy, OwnerInfo (CitizenNodeGuid, CharactersNodeData) and a progress save.
    void set_citizenship(Connection& conn, const svc::Citizenship& c, const char* why);
    /// One reliable call of the controller with `params` set by `fill` (false: it does not build, nothing goes out).
    void send_citizenship_call(Connection& conn, const char* rpc, std::function<bool(const protocol::schema::RpcSchema&, repl::RpcParams&)> fill);
    /// Takes `cost` Tax Receipts (the TaxVoucher items); the events go to the client and the bag is saved.  False with `why`, changing nothing, when the bags hold fewer.
    bool pay_tax_receipts(Connection& conn, int32_t cost, std::string& why);
    /// The account's dues now (world/citizenship.h): what one period costs at `now_ms`, with the late part once overdue; `factors` and `rank` (when given) get what
    /// it was made of.
    int32_t citizenship_dues(int64_t now_ms, world::CitizenshipRules::DuesFactors* factors = nullptr, int* rank = nullptr) const;
    /// The citizenship node's next dues date after `now_ms` (its schedule; a node with none yet: from the account's join, or now).
    int64_t node_dues_date(int64_t now_ms) const;
    /// Starts `node`'s dues schedule at `at_ms` if it has none (the store keeps it).
    void    start_dues_schedule(int64_t node, int64_t at_ms);
    /// Whether the dues for the node's coming date are paid (then the window shows them paid and a payment is refused).
    bool    dues_paid_ahead(int64_t now_ms) const;
    /// The highest level among the account's characters (HighestCharacterLevelOnAccount: the dues' level impact).
    int     account_highest_level() const;
    /// Every few seconds: the account's citizenship as another session of it changed it (adopted, not saved again), and a citizenship whose dues stayed unpaid too long lapses.
    void run_citizenship(Connection& conn);
    // ---- stage 2: mayors
    /// This character as the elections know a citizen, a candidate or a voter (the account, the character, its name).
    world::ElectionPerson election_person() const;
    /// The account's primary character as the elections' citizen.
    world::ElectionPerson citizen_person() const;
    /// The Sheriff's "stand for mayor" line (dialogue AddNodeMayorCandidate) and ServerAddNodeMayorCandidate: the character stands in its citizenship's node.
    void stand_for_mayor(Connection& conn, const char* by);
    void on_mayor_candidate(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_ranked_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_named_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_message(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_citizens(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// The elections' news for this session: OwnerInfo.MayorNode, the results of its node's election (ClientFinishMayorVote).
    void run_elections(Connection& conn, int64_t now_ms);
    /// ServerGMSetElectionStateAndDuration (the client's game master console; answered only with --gm-commands): the named node's election (NodeName empty: the node the
    /// player stands in) enters ElectionState for DurationInSeconds (0: the phase's own length).
    void on_gm_set_election(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// The node's election enters `state` (see on_gm_set_election).  What was done, or why nothing was.
    std::string gm_set_election(Connection& conn, const std::string& node_name, int state, int64_t duration_secs);
    /// The node at the NPC's place: its guid and level (0 and -1 for none).
    std::pair<int64_t, int> citizenship_node_at(double x, double y) const;
    svc::Citizenship                citizenship_;
    int64_t                         account_id_ = 0;   // the character's account: citizenship is the account's
    bool                           citizenship_changed_ = false;   // this session changed it: every progress save carries it
    uint64_t                        node_maps_election_version_ = 0;   // the elections' version the GameState's node maps were last brought to
    std::optional<CitizenshipOffer> citizenship_offer_;
    RequestBudget                   citizenship_budget_{kVendorBurst, kVendorInterval};
    size_t                          citizenship_log_ = 0;
    int64_t                         citizenship_checked_ms_ = 0;   // run_citizenship's last look
    uint64_t                        election_seen_version_ = 0;    // the elections' version run_elections last looked at
    int                             election_seen_state_ = -1;     // the state of the citizenship node's election then (-1: none)
    size_t                          election_seen_candidates_ = 0; // its candidates then (a change sends the window again)
    uint32_t                        election_windows_pushed_ = 0;  // windows sent for a phase or candidates change
    int64_t                         mayor_node_ = 0;               // OwnerInfo.MayorNode as last set
    uint32_t                        mayor_candidacies_ = 0, mayor_votes_ = 0, mayor_results_seen_ = 0;
    uint32_t citizenship_lapses_ = 0, citizenship_adopted_ = 0;
    uint32_t citizenship_joins_ = 0, citizenship_renounces_ = 0, citizenship_dues_paid_ = 0, citizenship_refused_ = 0, citizenship_windows_ = 0, citizenship_dialogs_ = 0;
    // ---- a game master's (session/player_citizenship_gm.cpp; the client's console, answered only for a game master: gm_rpc_admitted)
    void on_gm_set_mayor(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_set_mayor_candidate(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_set_mayor_message(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_add_node_citizen(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_remove_node_citizen(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_set_dues_due(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_end_renounce_cooldown(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_check_citizenship(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_citizenship_dues_switch(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now, bool off);
    void on_gm_mayor_vote_data(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_ranked_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// The node a game master's call names (a record or display name, any case; empty: the node the player stands in); 0 with `why` for none.
    int64_t gm_node_named(const std::string& name, std::string& why) const;
    /// The mayor's tools refuse a player who is no mayor: ClientShowMustBeMayorCallout.
    void send_must_be_mayor(Connection& conn);
public:
    /// A game master makes this account a citizen of `node` (ServerGMAddNodeCitizen): no cost, no level asked, the dues due a period on.  What was done.
    std::string gm_join_citizenship(Connection& conn, int64_t node, const std::string& by);
    /// A game master ends this account's citizenship (ServerGMRemoveNodeCitizen), with no renounce cooldown.  What was done.
    std::string gm_leave_citizenship(Connection& conn, const std::string& by);
    /// The account's citizenship as a game master reads it (ServerGMCheckCitizenshipStatus).
    std::string citizenship_status_text() const;
    const svc::Citizenship& citizenship() const { return citizenship_; }
    size_t   citizenship_requests() const { return citizenship_requests_; }   // the window's requests (ServerSetEssentialCitizenshipWindowData)
    uint32_t citizenship_joins() const { return citizenship_joins_; }
    uint32_t citizenship_renounces() const { return citizenship_renounces_; }
    uint32_t citizenship_dues_paid() const { return citizenship_dues_paid_; }
    uint32_t citizenship_refused() const { return citizenship_refused_; }
    uint32_t citizenship_windows() const { return citizenship_windows_; }
    uint32_t citizenship_dialogs() const { return citizenship_dialogs_; }
    uint32_t citizenship_lapses() const { return citizenship_lapses_; }
    uint32_t citizenship_adopted() const { return citizenship_adopted_; }
    uint32_t mayor_candidacies() const { return mayor_candidacies_; }
    uint32_t mayor_votes() const { return mayor_votes_; }
    uint32_t mayor_results_seen() const { return mayor_results_seen_; }
    uint32_t election_windows_pushed() const { return election_windows_pushed_; }
    int64_t  mayor_node() const { return mayor_node_; }
    /// A test: the game master's election change, past the --gm-commands gate.
    std::string gm_set_election_for_test(Connection& conn, const std::string& node_name, int state, int64_t duration_secs) {
        return gm_set_election(conn, node_name, state, duration_secs);
    }
    /// A test: run_citizenship now (not waiting for its few seconds).
    void run_citizenship_for_test(Connection& conn) { citizenship_checked_ms_ = 0; run_citizenship(conn); }
    /// A test: the elections as a new login first looks at them (the mayor's message comes again).
    void relogin_elections_for_test() {
        election_seen_version_ = 0;
        election_seen_state_ = -1;
    }
    /// The node of the standing offer (0: none).
    int64_t citizenship_offer_node() const { return citizenship_offer_ ? citizenship_offer_->node : 0; }
    /// A test: the Node Administrator's line for `npc`, as the conversation runs it.
    void open_citizenship_dialog_for_test(Connection& conn, const world::WorldActor& npc, bool join, int64_t vendor, Clock::time_point now) {
        open_citizenship_dialog(conn, npc, join, vendor, now);
    }
    /// A test: the account's citizenship as the join read it.
    void set_citizenship_for_test(const svc::Citizenship& c) { citizenship_ = c; }
private:

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/mail.inc: the mail cache, the jobs in flight and the admin mail entry (session/player_mail.inc)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// Mail: the live mails as the session last read them (the store's order) and the unread count.
    const std::vector<svc::Mail>& mails() const { return mails_; }
    int32_t unread_mail_count() const { return mail_unread_; }
    bool    mail_loaded() const { return mail_loaded_; }
    /// Mail: the worker jobs this session still waits for (a test runs ticks until it is 0).
    size_t  mail_pending() const { return mail_pending_.size(); }

    /// Mail: what the admin mail entry answers: the store's refusal, or the new mail's id and the character's unread count.
    /// A pending answer carries its ticket; take_gm_mail returns it when the create job lands.
    struct GmMailResult {
        bool        ok = false;
        std::string refusal;
        std::string mail_id;
        int32_t     unread = 0;
        bool        pending = false;
        uint64_t    ticket = 0;
    };
    using GmMailDone = std::function<void(uint64_t, const GmMailResult&)>;
    /// Mail: one mail to the character from the admin entry: the texts, and an item record with a count split into stacks (0: no item).
    /// The create runs on the world's worker; the answer is the returned pending ticket (take_gm_mail), or `when_done` when the job lands.
    GmMailResult gm_send_mail(Connection& conn, const std::string& sender, const std::string& subject, const std::string& body,
                              int64_t item_record, int32_t item_count, const std::string& by, GmMailDone when_done = {});
    /// Mail: the admin entry's answer when its create job landed; false while it has not.
    bool take_gm_mail(uint64_t ticket, GmMailResult& out);

    /// Mail: the sink the session reads and writes mails through; a test takes it away to reach a claim's no-sink guard.
    void set_mail_sink(PositionSink* sink) { sink_ = sink; }

private:
    /// What a request that needed the list does when the load lands.
    enum class MailWaitKind : uint8_t { None, Fetch, Refresh, Detail, Claim, ClaimAll, Status, BulkStatus, StaleClaim, StaleClaimAll };
    struct MailWait {
        MailWaitKind               kind = MailWaitKind::None;
        std::string                id;        // Detail, Claim, Status, StaleClaim
        int32_t                    item = 0;   // Claim
        int32_t                    status = 0; // Status/BulkStatus
        std::vector<std::string>   ids;        // ClaimAll/BulkStatus/StaleClaimAll
        std::vector<std::string>   guids;      // StaleClaim: the events to send when the store shows the claim
    };
    /// One attachment this request placed (a placement may carry several events: a top-up and a split).
    struct MailPlaced {
        std::string    guid;
        bool           moved = false;
        items::Outcome added;
    };
    /// A job the worker has and the session has not applied yet.
    struct PendingMail {
        uint64_t                   ticket = 0;
        MailJobKind                kind = MailJobKind::Load;
        std::vector<MailWait>      waits;      // Load: the requests that waited for it
        std::string                id;
        std::string                guid;       // Claim: the attachment's instance guid
        int32_t                    item = 0;
        std::vector<std::string>   ids;        // ClaimAll/BulkStatus
        std::vector<std::vector<size_t>> items; // ClaimAll: the item indices of each id
        std::vector<svc::Mail>     mails;      // ClaimAll/BulkStatus/SaveMail: the copies the worker writes
        std::vector<MailPlaced>    placed;     // Claim/ClaimAll: what the tick changed in the bags
        bool                       dirty_before = false, changed_before = false;
        GmMailDone                 gm_done;    // CreateMail: the admin entry's answer
    };

    void init_mail(const svc::Character& character);
    bool mail_refresh(const char* why);
    bool mail_start_load(const char* why, Clock::time_point now);
    bool mail_start_load(const char* why, MailWait wait, Clock::time_point now);
    bool mail_wait_for_load(MailWait wait, Clock::time_point now, const char* why);
    int  mail_find(const std::string& id) const;
    bool mail_place(const svc::InventoryItem& item, items::Outcome& added, std::string& why);
    void mail_roll_back(const items::Outcome& added, bool dirty_before, bool changed_before);
    void mail_send_headers(Connection& conn);
    void mail_send_unread(Connection& conn, const char* why);
    void mail_send_events(Connection& conn, std::vector<bootstrap::ItemEvent> events);
    void mail_send_added(Connection& conn, const std::vector<std::string>& guids, bool require_claimed);
    void mail_say_bags_full(Connection& conn);
    void mail_send_detail(Connection& conn, const std::string& id);
    void mail_send_status_answers(Connection& conn, const std::vector<std::pair<std::string, int32_t>>& answers);
    bool mail_claim_one(Connection& conn, size_t mail_index, int32_t item_index);
    bool mail_claim_all(Connection& conn, const std::vector<std::string>& ids);
    bool mail_bulk_status(Connection& conn, const std::vector<std::string>& ids, int32_t status);
    void mail_update_status(Connection& conn, const std::string& id, int32_t bits);

    void run_mail(Connection& conn, Clock::time_point now);
    void mail_apply(Connection& conn, Clock::time_point now, PendingMail job, MailJobAnswer answer);
    void mail_load_landed(Connection& conn, PendingMail job, const svc::MailsResult& loaded);
    void mail_claim_landed(Connection& conn, PendingMail job, const MailJobAnswer& answer);
    void mail_claim_all_landed(Connection& conn, PendingMail job, const MailJobAnswer& answer);
    void mail_status_landed(Connection& conn, PendingMail job, const MailJobAnswer& answer);
    void mail_bulk_landed(Connection& conn, PendingMail job, const MailJobAnswer& answer);
    void mail_create_landed(Connection& conn, PendingMail job, const svc::MailResult& made);
    void mail_run_wait(Connection& conn, const MailWait& wait);
    void close_mail(const char* why);

    void on_fetch_mail(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_fetch_mail_num(Connection& conn, const protocol::repl::ClientRpc& rpc);
    void on_get_mail_data(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_refresh_mail_headers(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_claim_mail_item(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_claim_mail_currency(Connection& conn, const protocol::repl::ClientRpc& rpc);
    void on_claim_all_mail(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_update_mail_status(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_bulk_update_mail_status(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);

    std::vector<svc::Mail> mails_;
    std::map<std::string, std::vector<bootstrap::ItemEvent>> mail_added_events_;   // the placement's events, until the mail save lands
    int32_t                mail_unread_ = 0;
    bool                   mail_loaded_ = false;
    uint64_t               mail_version_ = 0;
    RequestBudget          mail_refresh_budget_{1, std::chrono::seconds(10)};
    RequestBudget          mail_status_budget_{8, std::chrono::seconds(1)};
    size_t                 mail_logged_ = 0;
    size_t                 mail_refused_logged_ = 0;

    std::deque<PendingMail>        mail_pending_;          // the jobs in flight, oldest first
    std::set<std::string>          mail_claim_in_flight_;  // the mails whose claim the worker is writing
    std::map<uint64_t, GmMailResult> mail_gm_done_;        // the admin sends' answers, for a caller that polls
    uint32_t                       mail_load_failures_ = 0;
    Clock::time_point              mail_load_retry_at_{};  // the first moment a failed load may be tried again
    bool                           mail_load_in_flight_ = false;

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/mail_jobs.h: the mail calls a world's worker runs off the network thread (session/mail_jobs.inc has the worker).
//  The request carries copies of everything the worker needs, so a job never holds a session.
// ============================================================================
#pragma once

#include "session/position_saver.h"
#include "svc/services.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace aoc { namespace worldd {

/// What a mail job asks the character service.
enum class MailJobKind : uint8_t {
    Load,        // the character's mails
    SaveMail,    // one mail whole
    CreateMail,  // the admin send
    Claim,       // one mail's attachments: the bags and the claimed marks in one call
    ClaimAll,    // the attachments of several mails: the same one call
    BulkStatus,  // each mail named
    GmSend,      // the admin send to a character who is not in the world: the lookup by name, then the create
};

/// One job: the kind, the character and copies of the rows the worker writes.
struct MailJobRequest {
    MailJobKind                    kind = MailJobKind::Load;
    std::string                    character_id;
    svc::Mail                      mail;        // SaveMail
    svc::NewMail                   new_mail;    // CreateMail, GmSend
    std::string                    character_name;   // GmSend: the name to look up
    std::vector<svc::MailClaim>    claims;      // Claim/ClaimAll: the attachments to mark claimed
    std::vector<svc::MoneyClaim>   money_mails; // Claim/ClaimAll: the mails whose money the claim takes into the purse (`wallet` holds it), each with the copper the purse was credited for it
    std::vector<svc::InventoryItem> items;      // Claim/ClaimAll: the bags as the tick has them
    std::vector<svc::WalletEntry>  wallet;
    std::vector<svc::ProcessingJob> jobs;
    std::vector<int64_t>           recipes;
    std::vector<svc::WarehouseItem> warehouse;
    std::vector<svc::Mail>         mails;       // BulkStatus: the mails to save, in order
};

/// What the worker answered.
struct MailJobAnswer {
    MailJobKind                        kind = MailJobKind::Load;
    svc::MailsResult                   loaded;
    svc::MailClaimResult               claim;                        // Claim/ClaimAll: the answer of the one call (after the retries)
    std::vector<svc::MailSaveResult>   saves;                        // BulkStatus: one per mail the worker saved
    svc::MailResult                    created;                      // CreateMail, GmSend
    svc::CallStatus                    lookup = svc::CallStatus::Ok; // GmSend: the lookup's answer
    std::string                        lookup_name;                  // GmSend: the character's own spelling of the name
};

/// A claim whose session ended before the worker answered it.  The session cannot save its bags any more without racing the claim, so it hands the worker the two bags the
/// character can end with: `with` (the claim was stored) and `without` (it was not), and the worker saves the right one when the claim is answered.  A log-in of the character
/// waits for that (see MailJobs::claim_open and PlayerWorld::character_writes_pending), so no session starts from bags the answer can still change.
struct OrphanClaim {
    uint64_t                        ticket = 0;
    std::string                     character_id;
    std::vector<svc::InventoryItem> items_with, items_without;
    std::vector<svc::WalletEntry>   wallet;          // the purse the character ends with when the claim was stored (with the money, if it took any)
    std::vector<svc::WalletEntry>   wallet_without;  // ... and when it was not, when the claim took money (has_wallet_without); otherwise `wallet` is the purse in both cases
    bool                            has_wallet_without = false;
    std::vector<svc::ProcessingJob> jobs;
    std::vector<int64_t>            recipes;
    std::vector<svc::WarehouseItem> warehouse;
};

/// One worker thread for a world's mail calls (session/mail_jobs.inc).
class MailJobs {
public:
    /// A claim that has no answer (the service is down or slow) is never settled by a guess: the worker asks the SAME request again, first after `claim_retry_delay`, then after twice
    /// that and so on up to `claim_retry_cap`, until the service answers.  It is safe to ask again: the service answers a repeat of a stored claim Ok without storing it twice.  The
    /// wait is in the queue, not a sleep of the worker: the other jobs, of other characters, go on, and the character's own later jobs wait for the claim.
    /// A claim that stays unanswered is said loudly: a warning when it first gets no answer, then at most one in `claim_warn_every` while it stays so (it names the character and the
    /// mail and says that the character's bags are not saved until the service answers).  It never changes the retry rule.
    struct Config {
        std::chrono::milliseconds claim_retry_delay{300};
        std::chrono::milliseconds claim_retry_cap{10000};
        std::chrono::milliseconds claim_warn_every{60000};
    };
    MailJobs() : MailJobs(Config{}) {}
    explicit MailJobs(Config config);
    ~MailJobs();
    MailJobs(const MailJobs&) = delete;
    MailJobs& operator=(const MailJobs&) = delete;

    /// Queues `request` and returns its ticket.  The jobs run in the order they were submitted.  A claim of a character whose log-in is marked (login_began) is not queued and
    /// returns 0: the mark and the claim's registration are one step under the worker's lock, so a log-in that has marked its character and then looks for open claims either finds
    /// this one open or has this one refused, never neither.
    uint64_t submit(MailJobRequest request, PositionSink* sink);
    /// Takes a ticket's answer when the worker has it; false when it has not (or the ticket was forgotten).
    bool take(uint64_t ticket, MailJobAnswer& out);
    /// Drops a ticket and its answer: a session that ended never reads it.
    void forget(uint64_t ticket);
    /// Takes a job back that the worker has not started: true when it was still queued and never asked (it will never run).  False: it is running, waiting to ask again, or answered.
    /// A claim taken back stays open until claim_closed: its session closes it after the bags that follow are queued.
    bool cancel(uint64_t ticket);
    /// Leaves a claim the worker is running (or waiting to ask again; false when it was answered meanwhile: the caller takes the answer) to the worker: see OrphanClaim.
    bool orphan(OrphanClaim claim, PositionSink* sink);
    /// The session has applied a claim's answer: the claim is no longer open.
    void claim_closed(uint64_t ticket);
    /// Whether the character has a claim that is not closed: queued, running, waiting to ask again, answered and not applied yet, or left to the worker.
    bool claim_open(const std::string& character_id) const;
    /// A log-in of the character, whose ticket was redeemed for it, waits to be applied (`session_id` names it).  Set on the log-in's thread, read on the world's: both under the
    /// worker's lock.  Until login_ended the character's new claims are refused at submit.
    void login_began(const std::string& character_id, uint64_t session_id);
    void login_ended(uint64_t session_id);
    bool login_pending(const std::string& character_id) const;

private:
    struct Job {
        uint64_t        ticket = 0;
        MailJobRequest  request;
        PositionSink*   sink = nullptr;
        uint32_t        asks = 0;                                // how many times a claim was asked of the service
        bool            unclear = false;                         // a claim was asked and had no answer: it may have been stored
        bool            warned = false;                          // the warning of its missing answer was written at least once
        std::chrono::steady_clock::time_point last_warned{};     // and last at this time
        std::chrono::steady_clock::time_point not_before{};      // a claim without an answer is asked again then
    };

    void run();
    bool login_pending_locked(const std::string& character_id) const;

    mutable std::mutex             mu_;
    std::condition_variable        cv_;
    std::deque<Job>                jobs_;
    std::map<uint64_t, MailJobAnswer> done_;
    std::set<uint64_t>             forgotten_;
    std::map<uint64_t, OrphanClaim> orphans_;   // by ticket
    std::map<uint64_t, PositionSink*> orphan_sinks_;
    std::map<uint64_t, std::string> open_claims_;   // the claims not closed, by ticket: the character they are for
    std::map<uint64_t, std::string> logins_;        // the marked log-ins, by session id: the character they are for
    Config                         config_;
    bool                           stop_ = false;
    uint64_t                       next_ = 1;
    std::thread                    thread_;   // last: it starts once every other member is built
};

}}  // namespace aoc::worldd

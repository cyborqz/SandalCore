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
    Claim,       // the bags, then one mail
    ClaimAll,    // the bags, then each mail named
    BulkStatus,  // each mail named
};

/// One job: the kind, the character and copies of the rows the worker writes.
struct MailJobRequest {
    MailJobKind                    kind = MailJobKind::Load;
    std::string                    character_id;
    svc::Mail                      mail;        // SaveMail; Claim: the mail to save after the bags
    svc::NewMail                   new_mail;    // CreateMail
    std::vector<svc::InventoryItem> items;      // Claim/ClaimAll: the bags as the tick has them
    std::vector<svc::WalletEntry>  wallet;
    std::vector<svc::ProcessingJob> jobs;
    std::vector<int64_t>           recipes;
    std::vector<svc::WarehouseItem> warehouse;
    std::vector<svc::Mail>         mails;       // ClaimAll/BulkStatus: the mails to save, in order
};

/// What the worker answered.
struct MailJobAnswer {
    MailJobKind                        kind = MailJobKind::Load;
    svc::MailsResult                   loaded;
    svc::CallStatus                    bag = svc::CallStatus::Ok;   // Claim/ClaimAll: the bags' answer
    std::vector<svc::MailSaveResult>   saves;                        // one per mail the worker saved
    svc::MailResult                    created;
};

/// One worker thread for a world's mail calls (session/mail_jobs.inc).
class MailJobs {
public:
    MailJobs();
    ~MailJobs();
    MailJobs(const MailJobs&) = delete;
    MailJobs& operator=(const MailJobs&) = delete;

    /// Queues `request` and returns its ticket.  The jobs run in the order they were submitted.
    uint64_t submit(MailJobRequest request, PositionSink* sink);
    /// Takes a ticket's answer when the worker has it; false when it has not (or the ticket was forgotten).
    bool take(uint64_t ticket, MailJobAnswer& out);
    /// Drops a ticket and its answer: a session that ended never reads it.
    void forget(uint64_t ticket);

private:
    struct Job {
        uint64_t        ticket = 0;
        MailJobRequest  request;
        PositionSink*   sink = nullptr;
    };

    void run();

    std::mutex                     mu_;
    std::condition_variable        cv_;
    std::deque<Job>                jobs_;
    std::map<uint64_t, MailJobAnswer> done_;
    std::set<uint64_t>             forgotten_;
    bool                           stop_ = false;
    uint64_t                       next_ = 1;
    std::thread                    thread_;   // last: it starts once every other member is built
};

}}  // namespace aoc::worldd

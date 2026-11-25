// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/mail_jobs.inc
//
//  The mail worker of one world: the character service's mail calls may take seconds, and the network thread only submits
//  them and takes the answer on a later tick (review finding: a slow or down character service froze every player's tick).
//  One thread, one queue: the jobs run in the order they were submitted, so one character's claim writes its bags before
//  its mail and a status save does not overtake a claim.  A job holds copies of the rows and the sink, never a session.
// ============================================================================
#pragma once

namespace aoc { namespace worldd {

MailJobs::MailJobs() : thread_([this] { run(); }) {}

MailJobs::~MailJobs() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

uint64_t MailJobs::submit(MailJobRequest request, PositionSink* sink) {
    std::lock_guard<std::mutex> lock(mu_);
    const uint64_t ticket = next_++;
    if (stop_) {
        MailJobAnswer answer;
        answer.kind = request.kind;
        if (request.kind == MailJobKind::Load) answer.loaded.status = svc::CallStatus::Unavailable;
        else if (request.kind == MailJobKind::CreateMail) answer.created.status = svc::CallStatus::Unavailable;
        else if (request.kind == MailJobKind::Claim || request.kind == MailJobKind::ClaimAll) answer.bag = svc::CallStatus::Unavailable;
        else answer.saves.push_back(svc::MailSaveResult{svc::CallStatus::Unavailable, std::string()});
        done_[ticket] = std::move(answer);
        return ticket;
    }
    jobs_.push_back(Job{ticket, std::move(request), sink});
    cv_.notify_one();
    return ticket;
}

bool MailJobs::take(uint64_t ticket, MailJobAnswer& out) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = done_.find(ticket);
    if (it == done_.end()) return false;
    out = std::move(it->second);
    done_.erase(it);
    return true;
}

void MailJobs::forget(uint64_t ticket) {
    std::lock_guard<std::mutex> lock(mu_);
    done_.erase(ticket);
    forgotten_.insert(ticket);
}

void MailJobs::run() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        lock.unlock();

        MailJobAnswer answer;
        answer.kind = job.request.kind;
        switch (job.request.kind) {
        case MailJobKind::Load:
            if (job.sink) answer.loaded = job.sink->load_mails(job.request.character_id);
            else answer.loaded.status = svc::CallStatus::Failed;
            break;
        case MailJobKind::SaveMail:
            if (job.sink) answer.saves.push_back(job.sink->save_mail(job.request.character_id, job.request.mail));
            else answer.saves.push_back(svc::MailSaveResult{svc::CallStatus::Failed, std::string()});
            break;
        case MailJobKind::CreateMail:
            if (job.sink) answer.created = job.sink->create_mail(job.request.character_id, job.request.new_mail);
            else answer.created.status = svc::CallStatus::Failed;
            break;
        case MailJobKind::Claim:
            if (job.sink) {
                answer.bag = job.sink->save_inventory_now(job.request.character_id, job.request.items, job.request.wallet, job.request.jobs,
                                                          job.request.recipes, job.request.warehouse);
                if (answer.bag == svc::CallStatus::Ok) answer.saves.push_back(job.sink->save_mail(job.request.character_id, job.request.mail));
            } else {
                answer.bag = svc::CallStatus::Unavailable;
            }
            break;
        case MailJobKind::ClaimAll:
            if (job.sink) {
                answer.bag = job.sink->save_inventory_now(job.request.character_id, job.request.items, job.request.wallet, job.request.jobs,
                                                          job.request.recipes, job.request.warehouse);
                if (answer.bag == svc::CallStatus::Ok) {
                    for (const svc::Mail& mail : job.request.mails) {
                        const svc::MailSaveResult saved = job.sink->save_mail(job.request.character_id, mail);
                        answer.saves.push_back(saved);
                        if (saved.status == svc::CallStatus::Unavailable) break;
                    }
                }
            } else {
                answer.bag = svc::CallStatus::Unavailable;
            }
            break;
        case MailJobKind::BulkStatus:
            if (job.sink) {
                for (const svc::Mail& mail : job.request.mails) {
                    const svc::MailSaveResult saved = job.sink->save_mail(job.request.character_id, mail);
                    answer.saves.push_back(saved);
                    if (saved.status == svc::CallStatus::Unavailable) break;
                }
            } else {
                answer.bag = svc::CallStatus::Unavailable;
            }
            break;
        }

        lock.lock();
        const auto forgotten = forgotten_.find(job.ticket);
        if (forgotten != forgotten_.end()) {
            forgotten_.erase(forgotten);
        } else {
            done_[job.ticket] = std::move(answer);
        }
    }
}

uint64_t PlayerWorld::submit_mail_job(MailJobRequest request) const {
    if (!mail_jobs_) mail_jobs_ = std::make_unique<MailJobs>();
    return mail_jobs_->submit(std::move(request), sink_);
}

bool PlayerWorld::take_mail_job(uint64_t ticket, MailJobAnswer& out) const {
    return mail_jobs_ && mail_jobs_->take(ticket, out);
}

void PlayerWorld::forget_mail_job(uint64_t ticket) const {
    if (mail_jobs_) mail_jobs_->forget(ticket);
}

}}  // namespace aoc::worldd

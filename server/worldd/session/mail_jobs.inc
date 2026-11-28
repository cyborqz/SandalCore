// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/mail_jobs.inc
//
//  The mail worker of one world: the character service's mail calls may take seconds, and the network thread only submits
//  them and takes the answer on a later tick (review finding: a slow or down character service froze every player's tick).
//  One thread, one queue: the jobs run in the order they were submitted, so a status save does not overtake a claim and a load
//  after a claim reads it.  A claim is ONE call of the character service (the bags and the claimed marks together), asked again, the
//  same request, at a growing pace until it is answered (a claim without an answer is never settled by a guess).  A job holds copies
//  of the rows and the sink, never a session.
// ============================================================================
#pragma once

namespace aoc { namespace worldd {

MailJobs::MailJobs(Config config) : config_(config), thread_([this] { run(); }) {}

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
    const bool is_claim = request.kind == MailJobKind::Claim || request.kind == MailJobKind::ClaimAll;
    if (is_claim && login_pending_locked(request.character_id)) return 0;   // (a log-in of the character waits: see login_began)
    const uint64_t ticket = next_++;
    if (stop_) {
        MailJobAnswer answer;
        answer.kind = request.kind;
        if (request.kind == MailJobKind::Load) answer.loaded.status = svc::CallStatus::Unavailable;
        else if (request.kind == MailJobKind::CreateMail) answer.created.status = svc::CallStatus::Unavailable;
        else if (request.kind == MailJobKind::Claim || request.kind == MailJobKind::ClaimAll) answer.claim.status = svc::CallStatus::Unavailable;
        else if (request.kind == MailJobKind::GmSend) answer.lookup = svc::CallStatus::Unavailable;
        else answer.saves.push_back(svc::MailSaveResult{svc::CallStatus::Unavailable, std::string()});
        done_[ticket] = std::move(answer);
        return ticket;
    }
    if (is_claim) open_claims_[ticket] = request.character_id;
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

bool MailJobs::cancel(uint64_t ticket) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->ticket != ticket) continue;
        if (it->asks > 0) return false;   // it was asked once: the service may have it
        jobs_.erase(it);   // (the claim stays open: the session that took it back closes it after the bags that follow are queued)
        return true;
    }
    return false;
}

bool MailJobs::orphan(OrphanClaim claim, PositionSink* sink) {
    std::lock_guard<std::mutex> lock(mu_);
    if (done_.count(claim.ticket) != 0) return false;   // answered meanwhile: the session settles it itself
    const uint64_t ticket = claim.ticket;
    orphan_sinks_[ticket] = sink;
    orphans_[ticket] = std::move(claim);
    return true;
}

void MailJobs::claim_closed(uint64_t ticket) {
    std::lock_guard<std::mutex> lock(mu_);
    open_claims_.erase(ticket);
}

bool MailJobs::login_pending_locked(const std::string& character_id) const {
    for (const auto& [session, who] : logins_) {
        if (who == character_id) return true;
    }
    return false;
}

void MailJobs::login_began(const std::string& character_id, uint64_t session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    logins_[session_id] = character_id;
}

void MailJobs::login_ended(uint64_t session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    logins_.erase(session_id);
}

bool MailJobs::login_pending(const std::string& character_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return login_pending_locked(character_id);
}

bool MailJobs::claim_open(const std::string& character_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [ticket, who] : open_claims_) {
        if (who == character_id) return true;
    }
    return false;
}

void MailJobs::run() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        if (stop_) return;
        // The first job that may run now: it is not waiting to ask again, and no earlier job of the same character is (a character's jobs run in the order they came).
        const auto now = std::chrono::steady_clock::now();
        auto wake = std::chrono::steady_clock::time_point::max();
        std::set<std::string> held;
        auto pick = jobs_.end();
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
            const std::string& who = it->request.character_id;
            if (it->not_before > now) {
                held.insert(who);
                wake = std::min(wake, it->not_before);
            } else if (held.count(who) == 0) {
                pick = it;
                break;
            }
        }
        if (pick == jobs_.end()) {
            if (wake == std::chrono::steady_clock::time_point::max()) cv_.wait(lock);
            else cv_.wait_until(lock, wake);
            continue;
        }
        const size_t at = static_cast<size_t>(pick - jobs_.begin());
        Job job = std::move(*pick);
        jobs_.erase(pick);
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
        case MailJobKind::ClaimAll:
            if (job.sink) {
                answer.claim = job.sink->claim_mail(job.request.character_id, job.request.claims, job.request.money_mails, job.request.items, job.request.wallet, job.request.jobs,
                                                    job.request.recipes, job.request.warehouse);
                ++job.asks;
            } else {
                answer.claim.status = svc::CallStatus::Unavailable;
            }
            break;
        case MailJobKind::GmSend:
            if (!job.sink) {
                answer.lookup = svc::CallStatus::Unavailable;
                break;
            } else {
                const svc::CharacterResult found = job.sink->character_by_name(job.request.character_name);
                answer.lookup = found.status;
                answer.lookup_name = found.character.name;
                if (found.status != svc::CallStatus::Ok || found.character.id.empty()) {
                    if (found.status == svc::CallStatus::Ok) answer.lookup = svc::CallStatus::NotFound;
                    break;
                }
                answer.created = job.sink->create_mail(found.character.id, job.request.new_mail);
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
                answer.saves.push_back(svc::MailSaveResult{svc::CallStatus::Unavailable, std::string()});
            }
            break;
        }

        lock.lock();
        const bool is_claim = job.request.kind == MailJobKind::Claim || job.request.kind == MailJobKind::ClaimAll;
        // Unavailable and Failed say nothing about whether a claim was stored.  Its bags stay as they are, and the same request is asked again later, with a delay that grows to a cap.
        const bool unclear = is_claim && job.sink && (answer.claim.status == svc::CallStatus::Unavailable || answer.claim.status == svc::CallStatus::Failed);
        if (unclear && !stop_) {
            job.unclear = true;
            auto delay = config_.claim_retry_delay;
            for (uint32_t i = 1; i < job.asks && delay < config_.claim_retry_cap; ++i) delay *= 2;
            const auto now_asked = std::chrono::steady_clock::now();
            job.not_before = now_asked + std::min(delay, config_.claim_retry_cap);
            if (!job.warned || now_asked - job.last_warned >= config_.claim_warn_every) {
                job.warned = true;
                job.last_warned = now_asked;
                const std::string mail = job.request.claims.empty() ? std::string("(none)") : job.request.claims.front().mail_id;
                const size_t more = job.request.claims.size() > 1 ? job.request.claims.size() - 1 : 0;
                spdlog::warn("[mail] character {}: the claim of mail {}{} has no answer from the character service ({}): the character's bags are not saved until the service answers; "
                             "the same claim is asked again every {} ms or so (a world restart ends the wait; what is saved stays whole)",
                             job.request.character_id, mail, more ? " and " + std::to_string(more) + " more" : std::string(), svc::call_status_name(answer.claim.status),
                             static_cast<long long>(std::min(delay, config_.claim_retry_cap).count()));
            }
            jobs_.insert(jobs_.begin() + static_cast<std::ptrdiff_t>(std::min(at, jobs_.size())), std::move(job));
            continue;
        }
        // "Claimed already" always means this ask wrote nothing (a true repeat of a stored claim is answered Ok by the service), so it is a refusal like any other: the items go back.
        if (is_claim && job.unclear && answer.claim.status == svc::CallStatus::Refused && answer.claim.claimed_before) {
            spdlog::warn("[mail] character {}: the repeat of the claim of mail {} was answered 'claimed already' and is treated as a refusal: its items go back",
                         job.request.character_id, job.request.claims.empty() ? std::string("(none)") : job.request.claims.front().mail_id);
        }
        const bool answered = !is_claim || (answer.claim.status == svc::CallStatus::Ok || answer.claim.status == svc::CallStatus::Refused || answer.claim.status == svc::CallStatus::NotFound);
        if (is_claim && answered && job.warned) {
            spdlog::info("[mail] character {}: the claim that had no answer was answered ({}) after {} asks", job.request.character_id, svc::call_status_name(answer.claim.status), job.asks);
        }
        const auto forgotten = forgotten_.find(job.ticket);
        const auto orphan = orphans_.find(job.ticket);
        if (forgotten != forgotten_.end()) {
            forgotten_.erase(forgotten);
            if (answered) open_claims_.erase(job.ticket);
        } else if (orphan != orphans_.end()) {
            // The session ended with this claim in flight.  The worker saves the bags the answer decides (the one with the item when the claim was stored, the one without it
            // when it was refused), then the claim is closed: the save is queued first, so a log-in never sees the gap between them.  No answer, no save: nothing is guessed.
            if (answered) {
                OrphanClaim& left = orphan->second;
                PositionSink* sink = orphan_sinks_[job.ticket];
                const bool stored = answer.claim.status == svc::CallStatus::Ok;
                if (sink) {
                    const std::vector<svc::WalletEntry>& purse = (!stored && left.has_wallet_without) ? left.wallet_without : left.wallet;
                    sink->save_inventory(left.character_id, stored ? left.items_with : left.items_without, purse, left.jobs, left.recipes, left.warehouse);
                }
                open_claims_.erase(job.ticket);
            }
            orphan_sinks_.erase(job.ticket);
            orphans_.erase(orphan);
        } else {
            done_[job.ticket] = std::move(answer);
        }
    }
}

MailJobs* PlayerWorld::mail_jobs(bool create) const {
    std::lock_guard<std::mutex> lock(mail_jobs_mu_);   // (the log-in's thread asks too)
    if (!mail_jobs_ && create) mail_jobs_ = std::make_unique<MailJobs>(mail_job_config_);
    return mail_jobs_.get();
}

uint64_t PlayerWorld::submit_mail_job(MailJobRequest request) const {
    if (mail_submit_hook_) mail_submit_hook_();
    return mail_jobs(true)->submit(std::move(request), sink_);
}

bool PlayerWorld::take_mail_job(uint64_t ticket, MailJobAnswer& out) const {
    MailJobs* jobs = mail_jobs(false);
    return jobs && jobs->take(ticket, out);
}

void PlayerWorld::forget_mail_job(uint64_t ticket) const {
    if (MailJobs* jobs = mail_jobs(false)) jobs->forget(ticket);
}

bool PlayerWorld::cancel_mail_job(uint64_t ticket) const {
    MailJobs* jobs = mail_jobs(false);
    return jobs && jobs->cancel(ticket);
}

bool PlayerWorld::orphan_mail_claim(OrphanClaim claim) const {
    MailJobs* jobs = mail_jobs(false);
    return jobs && jobs->orphan(std::move(claim), sink_);
}

void PlayerWorld::close_mail_claim(uint64_t ticket) const {
    if (MailJobs* jobs = mail_jobs(false)) jobs->claim_closed(ticket);
}

bool PlayerWorld::mail_claim_open(const std::string& character_id) const {
    const MailJobs* jobs = mail_jobs(false);
    return jobs && jobs->claim_open(character_id);
}

bool PlayerWorld::character_writes_pending(const std::string& character_id) const {
    // The order is the point.  A listing's save without its answer, or the bags of an ended session whose listing was refused, are counted by the market until their bags are queued
    // in the sink (market/market_store.h releases the count after the queueing), so a character the market no longer counts has that write already visible to the sink's own check
    // below, and a write the saver still holds is never missed between the two.
    if (trade_saving(character_id)) return true;
    if (market_listings_.saving(character_id)) return true;
    const MailJobs* jobs = mail_jobs(false);
    if (jobs && jobs->claim_open(character_id)) return true;
    return sink_ && sink_->character_busy(character_id);
}

void PlayerWorld::login_began(const std::string& character_id, uint64_t session_id) const { mail_jobs(true)->login_began(character_id, session_id); }

void PlayerWorld::login_ended(uint64_t session_id) const {
    if (MailJobs* jobs = mail_jobs(false)) jobs->login_ended(session_id);
}

bool PlayerWorld::login_pending(const std::string& character_id) const {
    const MailJobs* jobs = mail_jobs(false);
    return jobs && jobs->login_pending(character_id);
}

}}  // namespace aoc::worldd

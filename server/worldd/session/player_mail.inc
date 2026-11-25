// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_mail.inc: the mailbox's window (the list, the detail and the status marks), the item claim, the admin send
//  and the unread count on the social component.  Included at the end of player_session.cpp (session/player_session.h has the members).
//
//  The character service's mail calls run on the world's worker (session/mail_jobs.inc): this file submits them and applies
//  their answers on a later tick, so a slow store never holds the network and tick thread.
// ============================================================================
namespace {

/// The four int32 words of the FGuid at `prefix` of a client call; nullopt when the call does not decode.
std::optional<std::array<uint32_t, 4>> mail_call_guid(const repl::ClientRpc& rpc, const char* prefix) {
    if (!rpc.call.decoded || !rpc.call.rpc) return std::nullopt;
    std::array<uint32_t, 4> words{};
    for (int i = 0; i < 4; ++i) {
        const std::string path = std::string(prefix) + "." + "ABCD"[i];
        const auto word = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path), 0);
        if (!word) return std::nullopt;
        words[static_cast<size_t>(i)] = static_cast<uint32_t>(*word);
    }
    return words;
}

/// The wire guid of a mail's four words.
repl::GuidValue mail_guid_of(const std::array<uint32_t, 4>& words) {
    return repl::GuidValue{words[0], words[1], words[2], words[3]};
}

/// The 32 hex digits of a mail id from its four words.
std::string mail_hex(const std::array<uint32_t, 4>& words) {
    return items::guid_hex(mail_guid_of(words));
}

/// The storage a bag event names.
bootstrap::ItemStorage mail_storage_of(svc::ItemContainer container) {
    switch (container) {
    case svc::ItemContainer::Equipment: return bootstrap::ItemStorage::Equipment;
    case svc::ItemContainer::Quest: return bootstrap::ItemStorage::Quest;
    case svc::ItemContainer::Resource0: return bootstrap::ItemStorage::Resource0;
    case svc::ItemContainer::Resource1: return bootstrap::ItemStorage::Resource1;
    case svc::ItemContainer::Resource2: return bootstrap::ItemStorage::Resource2;
    case svc::ItemContainer::Backpack: break;
    }
    return bootstrap::ItemStorage::Backpack;
}

/// One item as the client's storage event carries it (an Add of the bag's copy).
bootstrap::ItemEvent mail_item_event(const svc::InventoryItem& item) {
    bootstrap::ItemEvent event;
    event.type = repl::StorageEventType::Add;
    event.storage = mail_storage_of(item.container);
    event.slot = item.slot;
    event.item = items::to_instance(item);
    event.instigated = true;
    return event;
}

/// One inventory change as the client's storage event carries it.  The events a placement itself made are the ones the
/// client needs: a stack that topped up a held stack, or that was split over two slots, has no single bag row of its own.
bootstrap::ItemEvent mail_item_event(const items::Event& from) {
    bootstrap::ItemEvent event;
    event.type = from.type;
    event.storage = mail_storage_of(from.container);
    event.slot = from.slot;
    event.item = items::to_instance(from.item);
    event.instigated = from.instigated;
    event.count = from.count;
    return event;
}

}  // namespace

void PlayerSession::init_mail(const svc::Character& character) {
    for (const PendingMail& job : mail_pending_) {
        if (world_) world_->forget_mail_job(job.ticket);
    }
    mail_pending_.clear();
    mail_added_events_.clear();
    mail_claim_in_flight_.clear();
    mail_gm_done_.clear();
    mails_.clear();
    mail_loaded_ = false;
    mail_version_ = 0;
    mail_load_failures_ = 0;
    mail_load_retry_at_ = Clock::time_point{};
    mail_load_in_flight_ = false;
    mail_unread_ = std::max<int32_t>(0, character.unread_mail_count);   // svc::Character::unread_mail_count
    // The social component is not on the PlayerController here (join_sequence() adds it later): the count is sent by the first mail call.
    spdlog::log(level_for(mail_logged_), "[mail] {}: {} unread mail(s) from the character load", name(), mail_unread_);
}

bool PlayerSession::mail_refresh(const char* why) {
    return mail_start_load(why, last_now_);
}

bool PlayerSession::mail_start_load(const char* why, Clock::time_point now) {
    return mail_start_load(why, MailWait{}, now);
}

bool PlayerSession::mail_start_load(const char* why, MailWait wait, Clock::time_point now) {
    if (!sink_) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: no mail store is set: the cached list is kept", name());
        return false;
    }
    // A load already submitted (even behind a save of the same character) takes the request: the answer comes when it lands.
    for (PendingMail& job : mail_pending_) {
        if (job.kind != MailJobKind::Load) continue;
        if (wait.kind != MailWaitKind::None) job.waits.push_back(std::move(wait));
        return true;
    }
    if (now < mail_load_retry_at_) return false;   // (a failed load backs off: the cached list answers for now)
    MailJobRequest request;
    request.kind = MailJobKind::Load;
    request.character_id = character_id_;
    PendingMail job;
    job.kind = MailJobKind::Load;
    job.ticket = world_ ? world_->submit_mail_job(std::move(request)) : 0;
    if (!world_) return false;
    job.waits.push_back(std::move(wait));
    mail_pending_.push_back(std::move(job));
    mail_load_in_flight_ = true;
    return true;
}

bool PlayerSession::mail_wait_for_load(MailWait wait, Clock::time_point now, const char* why) {
    if (mail_loaded_ || !sink_) return false;
    return mail_start_load(why, std::move(wait), now);
}

void PlayerSession::run_mail(Connection& conn, Clock::time_point now) {
    if (!world_) return;
    while (!mail_pending_.empty()) {
        MailJobAnswer answer;
        if (!world_->take_mail_job(mail_pending_.front().ticket, answer)) break;   // (one character's answers come in order)
        PendingMail job = std::move(mail_pending_.front());
        mail_pending_.pop_front();
        mail_apply(conn, now, std::move(job), std::move(answer));
    }
}

void PlayerSession::mail_apply(Connection& conn, Clock::time_point now, PendingMail job, MailJobAnswer answer) {
    switch (job.kind) {
    case MailJobKind::Load:
        mail_load_landed(conn, std::move(job), answer.loaded);
        break;
    case MailJobKind::SaveMail:
        mail_status_landed(conn, std::move(job), answer);
        break;
    case MailJobKind::CreateMail:
        mail_create_landed(conn, std::move(job), answer.created);
        break;
    case MailJobKind::Claim:
        mail_claim_landed(conn, std::move(job), answer);
        break;
    case MailJobKind::ClaimAll:
        mail_claim_all_landed(conn, std::move(job), answer);
        break;
    case MailJobKind::BulkStatus:
        mail_bulk_landed(conn, std::move(job), answer);
        break;
    }
}

void PlayerSession::mail_load_landed(Connection& conn, PendingMail job, const svc::MailsResult& loaded) {
    mail_load_in_flight_ = false;
    const bool ok = loaded.status == svc::CallStatus::Ok;
    if (ok) {
        mails_.clear();
        mails_.reserve(loaded.mails.size());
        for (const svc::Mail& mail : loaded.mails) {
            if ((mail.status & svc::kMailStatusDeleted) == 0) mails_.push_back(mail);
        }
        mail_unread_ = svc::unread_count(mails_);
        mail_loaded_ = true;
        mail_load_failures_ = 0;
        ++mail_version_;
        spdlog::log(level_for(mail_logged_, 20), "[mail] {}: {} mail(s), {} unread ({}): version {}", name(), mails_.size(), mail_unread_, "the store", mail_version_);
    } else {
        ++mail_load_failures_;
        uint32_t seconds = 10;
        for (uint32_t i = 1; i < mail_load_failures_ && seconds < 60; ++i) seconds *= 2;
        if (seconds > 60) seconds = 60;
        mail_load_retry_at_ = last_now_ + std::chrono::seconds(seconds);
        // one line per back-off step, not one per request that finds the list missing
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the mail store did not answer the load ({}: {}): the cached list is kept and the next load waits {} s",
                    name(), svc::call_status_name(loaded.status), loaded.reason, seconds);
    }
    for (MailWait& wait : job.waits) mail_run_wait(conn, wait);
}

void PlayerSession::mail_run_wait(Connection& conn, const MailWait& wait) {
    switch (wait.kind) {
    case MailWaitKind::None:
        break;
    case MailWaitKind::Fetch:
    case MailWaitKind::Refresh:
        mail_send_headers(conn);
        mail_send_unread(conn, "the list was read");
        break;
    case MailWaitKind::Detail:
        mail_send_detail(conn, wait.id);
        break;
    case MailWaitKind::Claim: {
        const int found = mail_find(wait.id);
        if (found < 0) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the claim of {} is not in the cache: ignored", name(), wait.id);
            break;
        }
        mail_claim_one(conn, static_cast<size_t>(found), wait.item);
        break;
    }
    case MailWaitKind::ClaimAll:
        mail_claim_all(conn, wait.ids);
        break;
    case MailWaitKind::Status: {
        const int found = mail_find(wait.id);
        if (found < 0) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the status of {} is not in the cache: ignored", name(), wait.id);
            break;
        }
        mail_update_status(conn, wait.id, wait.status);
        break;
    }
    case MailWaitKind::BulkStatus:
        mail_bulk_status(conn, wait.ids, wait.status);
        break;
    case MailWaitKind::StaleClaim:
    case MailWaitKind::StaleClaimAll: {
        std::vector<std::string> guids;
        if (wait.kind == MailWaitKind::StaleClaim) guids = wait.guids;
        else {
            for (const auto& entry : mail_added_events_) guids.push_back(entry.first);
        }
        mail_send_added(conn, guids, /*require_claimed=*/true);
        break;
    }
    }
}

int PlayerSession::mail_find(const std::string& id) const {
    for (size_t i = 0; i < mails_.size(); ++i) {
        if (mails_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

bool PlayerSession::mail_place(const svc::InventoryItem& item, items::Outcome& added, std::string& why) {
    const std::optional<items::ItemInfo> info = item_catalog().info(item.item_record_id);
    if (!info) {
        why = "the item record is not in the catalog";
        return false;
    }
    if (info->quest && !info->wearable()) {
        // the Quest storage does not take add_at: the grant routes the item there
        added = inventory_.grant({item}, item_catalog());
    } else if (info->resource) {
        bool placed = false;
        for (int i = 0; i < items::kResourceContainers && !placed; ++i) {
            const svc::ItemContainer container = items::resource_container(i);
            const int slot = inventory_.place(container, info->inv_w, info->inv_h, item_catalog());
            if (slot < 0) continue;
            added = inventory_.add_at(container, slot, item, item_catalog());
            placed = true;
        }
        if (!placed) {
            why = "no resource bag has room";
            return false;
        }
    } else {
        const int slot = inventory_.first_free(svc::ItemContainer::Backpack);
        if (slot < 0) {
            why = "the item bag is full";
            return false;
        }
        added = inventory_.add_at(svc::ItemContainer::Backpack, slot, item, item_catalog());
    }
    if (!added.ok) {
        why = added.refusal.empty() ? "the bags refused the item" : added.refusal;
        return false;
    }
    return true;
}

void PlayerSession::mail_roll_back(const items::Outcome& added, bool dirty_before, bool changed_before) {
    for (auto it = added.events.rbegin(); it != added.events.rend(); ++it) {
        if (it->type != repl::StorageEventType::Add) continue;
        inventory_.consume(it->container, it->slot, it->item.instance_guid, it->item.quantity);
    }
    inventory_dirty_ = dirty_before;
    inventory_changed_ = changed_before;
}

void PlayerSession::mail_send_events(Connection& conn, std::vector<bootstrap::ItemEvent> events) {
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const size_t end = std::min(events.size(), at + bootstrap::kMaxEventsPerChain);
        std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at), events.begin() + static_cast<std::ptrdiff_t>(end));
        if (!room_for(1)) return;
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
}

void PlayerSession::mail_send_added(Connection& conn, const std::vector<std::string>& guids, bool require_claimed) {
    std::vector<bootstrap::ItemEvent> events;
    for (const std::string& guid : guids) {
        const auto it = mail_added_events_.find(guid);
        if (it == mail_added_events_.end()) continue;
        if (require_claimed) {
            bool claimed = false;
            for (const svc::Mail& mail : mails_) {
                for (const svc::MailItem& item : mail.items) claimed = claimed || (item.item.instance_guid == guid && item.claimed);
            }
            if (!claimed) continue;
        }
        for (const bootstrap::ItemEvent& event : it->second) events.push_back(event);
        mail_added_events_.erase(it);
    }
    if (!events.empty()) mail_send_events(conn, std::move(events));
}

void PlayerSession::mail_say_bags_full(Connection& conn) {
    const std::optional<bootstrap::Send> line = pb_.chat_line("Your bags are full.", 124, "Server");
    if (!line) {
        spdlog::log(level_for(mail_logged_), "[mail] {}: the full-bag line does not build", name());
        return;
    }
    if (!room_for(1)) return;
    queue("Your bags are full.", [send = *line](bootstrap::PlayerBootstrap&) { return send; }, conn, /*must_build=*/true);
}

void PlayerSession::mail_send_unread(Connection& conn, const char* why) {
    if (!room_for(1)) return;
    if (!pb_.set_num_unread_mail(mail_unread_)) return;   // (false before the social component: the next mail call sends it)
    const std::optional<bootstrap::Send> update = pb_.friends_update();
    if (!update) {
        spdlog::log(level_for(mail_logged_), "[mail] {}: the unread count {} does not build ({}): not sent", name(), mail_unread_, why);
        return;
    }
    queue("NumUnreadMail", [send = *update](bootstrap::PlayerBootstrap&) { return send; }, conn, /*must_build=*/true);
}

void PlayerSession::mail_send_headers(Connection& conn) {
    std::vector<repl::MailHeader> headers;
    headers.reserve(mails_.size());
    for (const svc::Mail& mail : mails_) {
        if ((mail.status & svc::kMailStatusDeleted) != 0) continue;
        repl::MailHeader header;
        header.guid = items::guid_of(mail.id).value_or(repl::GuidValue{});
        header.sender = mail.sender;
        header.subject = mail.subject;
        header.status = mail.status;
        header.expiration_time = mail.expires_at_ms;
        header.issue_time = mail.issued_at_ms;
        header.has_attachments = std::any_of(mail.items.begin(), mail.items.end(), [](const svc::MailItem& item) { return !item.claimed; });
        headers.push_back(std::move(header));
    }
    const std::optional<std::vector<bootstrap::Send>> pages = pb_.mail_headers(headers, mail_version_);
    if (!pages) {
        spdlog::log(level_for(mail_logged_), "[mail] {}: the mail headers do not build ({} header(s), version {}): nothing is sent", name(), headers.size(),
                    mail_version_);
        return;
    }
    if (!room_for(pages->size() + 1)) return;   // (the last step is the count that follows the list)
    for (const bootstrap::Send& page : *pages) {
        queue("ClientUpdateMailHeaders", [page](bootstrap::PlayerBootstrap&) { return page; }, conn, /*must_build=*/true);
    }
}

void PlayerSession::mail_send_detail(Connection& conn, const std::string& id) {
    const int found = mail_find(id);
    if (found < 0) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the detail of {} is not in the cache: ignored", name(), id);
        return;
    }
    const svc::Mail& mail = mails_[static_cast<size_t>(found)];
    repl::MailPersistenceData data;
    data.guid = items::guid_of(mail.id).value_or(repl::GuidValue{});
    data.subject = mail.subject;
    data.created_time = mail.issued_at_ms;
    data.expiration_time = mail.expires_at_ms;
    data.sender = mail.sender;
    data.message.title = mail.subject;
    data.message.body = mail.body;
    data.status = mail.status;
    data.items.reserve(mail.items.size());
    for (const svc::MailItem& item : mail.items) data.items.push_back(repl::MailItem{items::to_instance(item.item), item.claimed});
    const std::optional<bootstrap::Send> answer = pb_.mail_data(data);
    if (!answer) {
        spdlog::log(level_for(mail_logged_), "[mail] {}: the detail of {} does not build", name(), id);
        return;
    }
    if (!room_for(1)) return;
    queue("ClientGetMailData", [send = *answer](bootstrap::PlayerBootstrap&) { return send; }, conn, /*must_build=*/true);
}

void PlayerSession::mail_send_status_answers(Connection& conn, const std::vector<std::pair<std::string, int32_t>>& answers) {
    // one ClientUpdateMailStatus chain per at most kMaxMailStatusUpdates mails of the same final status
    for (size_t at = 0; at < answers.size();) {
        size_t end = at;
        while (end < answers.size() && answers[end].second == answers[at].second && end - at < bootstrap::kMaxMailStatusUpdates) ++end;
        std::vector<repl::GuidValue> guids;
        guids.reserve(end - at);
        for (size_t i = at; i < end; ++i) {
            if (const auto guid = items::guid_of(answers[i].first)) guids.push_back(*guid);
        }
        const std::optional<bootstrap::Send> answer = pb_.mail_status_update(guids, static_cast<uint32_t>(answers[at].second));
        if (!answer) {
            spdlog::log(level_for(mail_logged_), "[mail] {}: the status answer for {} mail(s) does not build", name(), guids.size());
        } else {
            queue("ClientUpdateMailStatus", [send = *answer](bootstrap::PlayerBootstrap&) { return send; }, conn, /*must_build=*/true);
        }
        at = end;
    }
}

void PlayerSession::mail_update_status(Connection& conn, const std::string& id, int32_t bits) {
    const int found = mail_find(id);
    if (found < 0) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the status of {} is not in the cache: ignored", name(), id);
        return;
    }
    const svc::Mail& mail = mails_[static_cast<size_t>(found)];
    if ((mail.status & bits) == bits) {
        // the mark is already there: no write at all, only the answer the client asked for
        mail_send_status_answers(conn, {{id, mail.status}});
        mail_send_unread(conn, "a mail status was already set");
        return;
    }
    MailJobRequest request;
    request.kind = MailJobKind::SaveMail;
    request.character_id = character_id_;
    PendingMail job;
    job.kind = MailJobKind::SaveMail;
    job.id = id;
    job.mails.push_back(mails_[static_cast<size_t>(found)]);
    job.mails.back().status |= bits;
    request.mail = job.mails.back();
    job.ticket = world_ ? world_->submit_mail_job(std::move(request)) : 0;
    if (!world_) return;
    mail_pending_.push_back(std::move(job));
}

void PlayerSession::mail_status_landed(Connection& conn, PendingMail job, const MailJobAnswer& answer) {
    if (answer.saves.empty()) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the status save of {} has no answer: the cache is kept", name(), job.id);
        return;
    }
    const svc::MailSaveResult& saved = answer.saves.front();
    if (saved.status == svc::CallStatus::Ok) {
        const int found = mail_find(job.id);
        const int32_t final_status = job.mails.empty() ? 0 : job.mails.front().status;
        if (found >= 0) {
            mails_[static_cast<size_t>(found)].status = final_status;
            if ((final_status & svc::kMailStatusDeleted) != 0) {
                mails_.erase(mails_.begin() + static_cast<std::ptrdiff_t>(found));
            }
        }
        mail_unread_ = svc::unread_count(mails_);
        ++mail_version_;
        mail_send_status_answers(conn, {{job.id, final_status}});
        mail_send_unread(conn, "a mail status was saved");
        return;
    }
    spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the status save of {} did not answer Ok ({}: {}): the cache is kept", name(), job.id,
                svc::call_status_name(saved.status), saved.reason);
    if (saved.status == svc::CallStatus::Failed || saved.status == svc::CallStatus::NotFound) {
        mail_start_load("a mail status save was stale", MailWait{}, last_now_);
    }
}

bool PlayerSession::mail_bulk_status(Connection& conn, const std::vector<std::string>& ids, int32_t status) {
    if (status == 0 || ids.empty()) return false;
    std::vector<std::pair<std::string, int32_t>> no_save;
    PendingMail job;
    job.kind = MailJobKind::BulkStatus;
    for (const std::string& id : ids) {
        const int found = mail_find(id);
        if (found < 0) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: a bulk status names a mail the cache does not hold ({}): skipped", name(), id);
            continue;
        }
        const svc::Mail& mail = mails_[static_cast<size_t>(found)];
        if ((mail.status & status) == status) {
            no_save.push_back({id, mail.status});
            continue;
        }
        svc::Mail next = mail;
        next.status |= status;
        job.ids.push_back(id);
        job.mails.push_back(std::move(next));
    }
    if (!no_save.empty()) {
        mail_send_status_answers(conn, no_save);
        mail_send_unread(conn, "a bulk status was already set");
    }
    if (job.mails.empty()) return !no_save.empty();
    MailJobRequest request;
    request.kind = MailJobKind::BulkStatus;
    request.character_id = character_id_;
    request.mails = job.mails;
    job.ticket = world_ ? world_->submit_mail_job(std::move(request)) : 0;
    if (!world_) return !no_save.empty();
    mail_pending_.push_back(std::move(job));
    return true;
}

void PlayerSession::mail_bulk_landed(Connection& conn, PendingMail job, const MailJobAnswer& answer) {
    std::vector<std::pair<std::string, int32_t>> answered;
    bool stale = false;
    for (size_t i = 0; i < job.ids.size() && i < answer.saves.size(); ++i) {
        const svc::MailSaveResult& saved = answer.saves[i];
        if (saved.status == svc::CallStatus::Unavailable) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the bulk status stopped at {} ({}): the later mails are not saved or answered", name(), job.ids[i],
                        svc::call_status_name(saved.status));
            break;
        }
        if (saved.status != svc::CallStatus::Ok) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the bulk status did not save {} ({}: {}): it stays unmarked", name(), job.ids[i],
                        svc::call_status_name(saved.status), saved.reason);
            if (saved.status == svc::CallStatus::Failed || saved.status == svc::CallStatus::NotFound) stale = true;
            continue;
        }
        const int found = mail_find(job.ids[i]);
        int32_t final_status = job.mails[i].status;
        if (found >= 0) {
            svc::Mail& mail = mails_[static_cast<size_t>(found)];
            mail = job.mails[i];
            final_status = mail.status;
            if ((mail.status & svc::kMailStatusDeleted) != 0) mails_.erase(mails_.begin() + static_cast<std::ptrdiff_t>(found));
            ++mail_version_;
        }
        answered.push_back({job.ids[i], final_status});
    }
    if (!answered.empty()) {
        mail_send_status_answers(conn, answered);
        mail_unread_ = svc::unread_count(mails_);
        mail_send_unread(conn, "a bulk status was saved");
    }
    if (stale) mail_start_load("a bulk status' mail save was stale", MailWait{}, last_now_);
}

bool PlayerSession::mail_claim_one(Connection& conn, size_t mail_index, int32_t item_index) {
    if (mail_index >= mails_.size()) return false;
    const std::string id = mails_[mail_index].id;
    if (item_index < 0 || static_cast<size_t>(item_index) >= mails_[mail_index].items.size()) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the claim names item {} of a mail with {} item(s): ignored", name(), item_index,
                    mails_[mail_index].items.size());
        return false;
    }
    const svc::MailItem& attachment = mails_[mail_index].items[static_cast<size_t>(item_index)];
    if (mail_claim_in_flight_.count(id) != 0) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: item {} of {} is already claimed: ignored", name(), item_index, id);
        return false;
    }
    if (attachment.claimed) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: item {} of {} is already claimed: ignored", name(), item_index, id);
        return false;
    }
    if (!sink_) {
        // (before the bag change: a session without a store must not move an item it cannot mark)
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: no mail store is set: the claim of {} is ignored", name(), id);
        return false;
    }
    const std::string guid = attachment.item.instance_guid;
    // an attachment whose placement is still waiting for its mail save is already placed: a retry must not grant it again
    // (a top-up leaves no bag row under the attachment's guid, so the events are what says it is there)
    const bool held = inventory_.find_guid(guid) != nullptr || mail_added_events_.count(guid) != 0;
    const bool in_warehouse = !held && wh_storage_.holds_guid(guid);
    PendingMail job;
    job.kind = MailJobKind::Claim;
    job.id = id;
    job.item = item_index;
    job.guid = guid;
    job.dirty_before = inventory_dirty_;
    job.changed_before = inventory_changed_;
    if (!held && !in_warehouse) {
        items::Outcome added;
        std::string why;
        if (!mail_place(attachment.item, added, why)) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: item {} of {} does not fit the bags ({}): not claimed", name(), item_index, id, why);
            mail_say_bags_full(conn);
            return false;
        }
        inventory_dirty_ = true;
        inventory_changed_ = true;
        job.placed.push_back(MailPlaced{guid, true, std::move(added)});
        std::vector<bootstrap::ItemEvent> events;
        for (const items::Event& event : job.placed.back().added.events) events.push_back(mail_item_event(event));
        mail_added_events_[guid] = std::move(events);
    }
    svc::Mail next = mails_[mail_index];
    next.items[static_cast<size_t>(item_index)].claimed = true;
    MailJobRequest request;
    request.kind = MailJobKind::Claim;
    request.character_id = character_id_;
    request.mail = std::move(next);
    request.items = inventory_.items();
    request.wallet = wallet_.entries();
    request.jobs = jobs_;
    request.recipes = taught_;
    request.warehouse = wh_storage_.rows();
    job.ticket = world_ ? world_->submit_mail_job(std::move(request)) : 0;
    if (!world_) return false;
    mail_claim_in_flight_.insert(id);
    mail_pending_.push_back(std::move(job));
    return true;
}

void PlayerSession::mail_claim_landed(Connection& conn, PendingMail job, const MailJobAnswer& answer) {
    mail_claim_in_flight_.erase(job.id);
    if (answer.bag == svc::CallStatus::Refused || answer.bag == svc::CallStatus::NotFound) {
        for (auto it = job.placed.rbegin(); it != job.placed.rend(); ++it) {
            if (it->moved) mail_roll_back(it->added, job.dirty_before, job.changed_before);
            mail_added_events_.erase(it->guid);
        }
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the bags refused the claim of {} ({}): the item is not moved", name(), job.id,
                    svc::call_status_name(answer.bag));
        return;
    }
    if (answer.bag != svc::CallStatus::Ok) {
        // not a clean refusal: the store may have written the bags; keep the item where the session put it and retry the mail later
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the bags did not answer the claim of {} ({}): the bag change is kept and the mail stays unclaimed", name(),
                    job.id, svc::call_status_name(answer.bag));
        return;
    }
    if (answer.saves.empty()) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the claim of {} has no mail answer: the bag change is kept and the mail stays unclaimed", name(), job.id);
        return;
    }
    const svc::MailSaveResult& saved = answer.saves.front();
    if (saved.status == svc::CallStatus::Ok) {
        const int found = mail_find(job.id);
        if (found >= 0 && static_cast<size_t>(job.item) < mails_[static_cast<size_t>(found)].items.size()) {
            mails_[static_cast<size_t>(found)].items[static_cast<size_t>(job.item)].claimed = true;
            ++mail_version_;
        }
        mail_send_added(conn, {job.guid}, /*require_claimed=*/false);
        const auto guid = items::guid_of(job.id);
        const std::optional<bootstrap::Send> answer_send = guid ? pb_.mail_item_claimed(*guid, job.item) : std::nullopt;
        if (!answer_send) {
            spdlog::log(level_for(mail_logged_), "[mail] {}: the claim answer of {} does not build", name(), job.id);
        } else {
            queue("ClientClaimMailItem", [send = *answer_send](bootstrap::PlayerBootstrap&) { return send; }, conn, /*must_build=*/true);
        }
        spdlog::log(level_for(mail_logged_, 20), "[mail] {}: item {} of {} is claimed", name(), job.item, job.id);
        return;
    }
    spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the claim of item {} of {} did not save the mail ({}: {}): it stays unclaimed", name(), job.item, job.id,
                svc::call_status_name(saved.status), saved.reason);
    if (saved.status == svc::CallStatus::Failed || saved.status == svc::CallStatus::NotFound) {
        // the store may already show the claim (a second session): read it again, and send what this session added when it does
        MailWait wait;
        wait.kind = MailWaitKind::StaleClaim;
        wait.id = job.id;
        wait.item = job.item;
        for (const MailPlaced& placed : job.placed) wait.guids.push_back(placed.guid);
        if (wait.guids.empty()) wait.guids.push_back(job.guid);
        mail_start_load("a claim's mail save was stale", std::move(wait), last_now_);
    }
}

bool PlayerSession::mail_claim_all(Connection& conn, const std::vector<std::string>& ids) {
    if (!sink_) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: no mail store is set: the claim-all is ignored", name());
        return false;
    }
    if (ids.empty()) return false;
    PendingMail job;
    job.kind = MailJobKind::ClaimAll;
    job.dirty_before = inventory_dirty_;
    job.changed_before = inventory_changed_;
    // every item of the request that the cache holds and has not claimed; a mail with a claim in flight is left alone
    for (const std::string& id : ids) {
        const int found = mail_find(id);
        if (found < 0) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the claim-all names a mail the cache does not hold ({}): skipped", name(), id);
            continue;
        }
        const size_t mail_index = static_cast<size_t>(found);
        if (mail_claim_in_flight_.count(id) != 0) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: a claim of {} is already in flight: the mail is skipped", name(), id);
            continue;
        }
        std::vector<size_t> items;
        for (size_t item = 0; item < mails_[mail_index].items.size(); ++item) {
            const svc::MailItem& attachment = mails_[mail_index].items[item];
            if (attachment.claimed) continue;
            const std::string& guid = attachment.item.instance_guid;
            if (inventory_.find_guid(guid) == nullptr && mail_added_events_.count(guid) == 0 && !wh_storage_.holds_guid(guid)) {
                items::Outcome added;
                std::string why;
                if (!mail_place(attachment.item, added, why)) {
                    // all or nothing: undo every add this request made, say it once, and save nothing
                    for (auto it = job.placed.rbegin(); it != job.placed.rend(); ++it) {
                        if (it->moved) mail_roll_back(it->added, job.dirty_before, job.changed_before);
                        mail_added_events_.erase(it->guid);
                    }
                    inventory_dirty_ = job.dirty_before;
                    inventory_changed_ = job.changed_before;
                    spdlog::log(level_for(mail_refused_logged_), "[mail] {}: item {} of {} does not fit the bags ({}): the claim-all is not made", name(), item, id,
                                why);
                    mail_say_bags_full(conn);
                    return false;
                }
                inventory_dirty_ = true;
                inventory_changed_ = true;
                job.placed.push_back(MailPlaced{guid, true, std::move(added)});
                std::vector<bootstrap::ItemEvent> events;
                for (const items::Event& event : job.placed.back().added.events) events.push_back(mail_item_event(event));
                mail_added_events_[guid] = std::move(events);
            }
            items.push_back(item);
        }
        if (items.empty()) continue;
        svc::Mail next = mails_[mail_index];
        for (size_t item : items) next.items[item].claimed = true;
        job.ids.push_back(id);
        job.items.push_back(std::move(items));
        job.mails.push_back(std::move(next));
    }
    if (job.mails.empty()) return false;
    MailJobRequest request;
    request.kind = MailJobKind::ClaimAll;
    request.character_id = character_id_;
    request.mails = job.mails;
    request.items = inventory_.items();
    request.wallet = wallet_.entries();
    request.jobs = jobs_;
    request.recipes = taught_;
    request.warehouse = wh_storage_.rows();
    job.ticket = world_ ? world_->submit_mail_job(std::move(request)) : 0;
    if (!world_) return false;
    for (const std::string& id : job.ids) mail_claim_in_flight_.insert(id);
    mail_pending_.push_back(std::move(job));
    return true;
}

void PlayerSession::mail_claim_all_landed(Connection& conn, PendingMail job, const MailJobAnswer& answer) {
    for (const std::string& id : job.ids) mail_claim_in_flight_.erase(id);
    if (answer.bag == svc::CallStatus::Refused || answer.bag == svc::CallStatus::NotFound) {
        for (auto it = job.placed.rbegin(); it != job.placed.rend(); ++it) {
            if (it->moved) mail_roll_back(it->added, job.dirty_before, job.changed_before);
            mail_added_events_.erase(it->guid);
        }
        inventory_dirty_ = job.dirty_before;
        inventory_changed_ = job.changed_before;
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the bags refused the claim-all ({}): nothing is moved", name(), svc::call_status_name(answer.bag));
        return;
    }
    if (answer.bag != svc::CallStatus::Ok) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the bags did not answer the claim-all ({}): the bag change is kept and the mails stay unclaimed", name(),
                    svc::call_status_name(answer.bag));
        return;
    }
    bool any = false;
    bool stale = false;
    for (size_t i = 0; i < job.ids.size() && i < answer.saves.size(); ++i) {
        const svc::MailSaveResult& saved = answer.saves[i];
        if (saved.status == svc::CallStatus::Unavailable) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the claim-all stopped at {} ({}): the later mails are not saved or answered", name(), job.mails[i].id,
                        svc::call_status_name(saved.status));
            break;
        }
        if (saved.status != svc::CallStatus::Ok) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the claim-all did not save {} ({}: {}): it stays unclaimed", name(), job.mails[i].id,
                        svc::call_status_name(saved.status), saved.reason);
            if (saved.status == svc::CallStatus::Failed || saved.status == svc::CallStatus::NotFound) stale = true;
            continue;
        }
        const int found = mail_find(job.ids[i]);
        if (found >= 0) {
            mails_[static_cast<size_t>(found)] = job.mails[i];
            ++mail_version_;
        }
        any = true;
        std::vector<std::string> guids;
        for (size_t item : job.items[i]) {
            if (job.mails[i].items.size() <= item) continue;
            guids.push_back(job.mails[i].items[item].item.instance_guid);
        }
        mail_send_added(conn, guids, /*require_claimed=*/false);
        for (size_t item : job.items[i]) {
            const auto guid = items::guid_of(job.mails[i].id);
            const std::optional<bootstrap::Send> answer_send = guid ? pb_.mail_item_claimed(*guid, static_cast<int32_t>(item)) : std::nullopt;
            if (answer_send) {
                queue("ClientClaimMailItem", [send = *answer_send](bootstrap::PlayerBootstrap&) { return send; }, conn, /*must_build=*/true);
            } else {
                spdlog::log(level_for(mail_logged_), "[mail] {}: the claim answer of {} (item {}) does not build", name(), job.mails[i].id, item);
            }
        }
    }
    if (any) mail_send_unread(conn, "a claim-all was saved");
    if (stale) {
        MailWait wait;
        wait.kind = MailWaitKind::StaleClaimAll;
        mail_start_load("a claim-all's mail save was stale", std::move(wait), last_now_);
    }
}

void PlayerSession::on_fetch_mail(Connection& conn, const repl::ClientRpc&, Clock::time_point now) {
    if (!mail_loaded_) {
        MailWait wait;
        wait.kind = MailWaitKind::Fetch;
        if (mail_start_load("the client asked for the list", std::move(wait), now)) return;
    }
    mail_send_headers(conn);
    mail_send_unread(conn, "the client asked for the list");
}

void PlayerSession::on_fetch_mail_num(Connection& conn, const repl::ClientRpc&) {
    mail_send_unread(conn, "the client asked for the count");
}

void PlayerSession::on_get_mail_data(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::optional<std::array<uint32_t, 4>> guid = mail_call_guid(rpc, "MailGuid");
    if (!guid) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: a mail detail names no mail: ignored", name());
        return;
    }
    const std::string id = mail_hex(*guid);
    if (mail_find(id) < 0 && !mail_loaded_) {
        MailWait wait;
        wait.kind = MailWaitKind::Detail;
        wait.id = id;
        if (mail_start_load("the client asked for a mail's detail", std::move(wait), now)) return;
    }
    mail_send_detail(conn, id);
}

void PlayerSession::on_refresh_mail_headers(Connection& conn, const repl::ClientRpc&, Clock::time_point now) {
    if (!within_budget(mail_refresh_budget_, "ServerRefreshMailHeaders", now)) return;
    MailWait wait;
    wait.kind = MailWaitKind::Refresh;
    if (mail_start_load("the client asked for a refresh", std::move(wait), now)) return;
    mail_send_headers(conn);
    mail_send_unread(conn, "the client asked for a refresh");
}

void PlayerSession::on_claim_mail_item(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::optional<std::array<uint32_t, 4>> guid = mail_call_guid(rpc, "MailGuid");
    if (!guid) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: a claim names no mail: ignored", name());
        return;
    }
    const int32_t item_index = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "MailItemIndex"), 0).value_or(0);
    const std::string id = mail_hex(*guid);
    if (mail_find(id) < 0 && !mail_loaded_) {
        MailWait wait;
        wait.kind = MailWaitKind::Claim;
        wait.id = id;
        wait.item = item_index;
        if (mail_start_load("the client claimed an item", std::move(wait), now)) return;
    }
    const int found = mail_find(id);
    if (found < 0) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the claim of {} is not in the cache: ignored", name(), id);
        return;
    }
    mail_claim_one(conn, static_cast<size_t>(found), item_index);
}

void PlayerSession::on_claim_mail_currency(Connection& conn, const repl::ClientRpc& rpc) {
    (void)conn;
    (void)rpc;
    spdlog::log(level_for(mail_logged_), "[mail] {}: a mail currency claim is ignored: the mails carry no money", name());
}

void PlayerSession::on_claim_all_mail(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!rpc.call.decoded || !rpc.call.rpc) return;
    const repl::Value* value = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "MailGuids");
    const repl::ArrayValue* list = value ? value->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.empty()) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: a claim-all names no mails: ignored", name());
        return;
    }
    std::vector<std::string> ids;
    std::set<std::string> seen;
    for (const repl::ElementState& element : list->elements) {
        std::array<uint32_t, 4> words{};
        bool whole = true;
        for (uint16_t i = 1; i <= 4 && whole; ++i) {
            const auto it = element.values.find(i);
            const auto word = it == element.values.end() ? std::nullopt : repl::rpcf::int_or(&it->second, 0);
            whole = word.has_value();
            if (word) words[i - 1] = static_cast<uint32_t>(*word);
        }
        if (!whole) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: a claim-all entry names no mail: ignored", name());
            continue;
        }
        const std::string id = mail_hex(words);
        if (seen.insert(id).second) ids.push_back(id);
    }
    if (ids.empty()) return;
    bool missing = false;
    for (const std::string& id : ids) missing = missing || mail_find(id) < 0;
    if (missing && !mail_loaded_) {
        MailWait wait;
        wait.kind = MailWaitKind::ClaimAll;
        wait.ids = ids;
        if (mail_start_load("the client claimed all mail", std::move(wait), now)) return;
    }
    mail_claim_all(conn, ids);
}

void PlayerSession::on_update_mail_status(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::optional<std::array<uint32_t, 4>> guid = mail_call_guid(rpc, "MailGuid");
    if (!guid) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: a status names no mail: ignored", name());
        return;
    }
    const int32_t status = static_cast<int32_t>(repl::rpcf::uint_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Status"), 0).value_or(0));
    if (status == 0) return;
    if (!within_budget(mail_status_budget_, "ServerUpdateMailStatus", now)) return;
    const std::string id = mail_hex(*guid);
    if (mail_find(id) < 0 && !mail_loaded_) {
        MailWait wait;
        wait.kind = MailWaitKind::Status;
        wait.id = id;
        wait.status = status;
        if (mail_start_load("the client marked a mail", std::move(wait), now)) return;
    }
    mail_update_status(conn, id, status);
}

void PlayerSession::on_bulk_update_mail_status(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!rpc.call.decoded || !rpc.call.rpc) return;
    const int32_t status = static_cast<int32_t>(repl::rpcf::uint_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Status"), 0).value_or(0));
    if (status == 0) return;
    if (!within_budget(mail_status_budget_, "ServerBulkUpdateMailStatus", now)) return;
    const repl::Value* value = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "MailGuids");
    const repl::ArrayValue* list = value ? value->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.empty()) {
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: a bulk status names no mails: ignored", name());
        return;
    }
    std::vector<std::string> ids;
    std::set<std::string> seen;
    for (const repl::ElementState& element : list->elements) {
        std::array<uint32_t, 4> words{};
        bool whole = true;
        for (uint16_t i = 1; i <= 4 && whole; ++i) {
            const auto it = element.values.find(i);
            const auto word = it == element.values.end() ? std::nullopt : repl::rpcf::int_or(&it->second, 0);
            whole = word.has_value();
            if (word) words[i - 1] = static_cast<uint32_t>(*word);
        }
        if (!whole) {
            spdlog::log(level_for(mail_refused_logged_), "[mail] {}: a bulk status entry names no mail: ignored", name());
            continue;
        }
        const std::string id = mail_hex(words);
        if (seen.insert(id).second) ids.push_back(id);
    }
    if (ids.empty()) return;
    bool missing = false;
    for (const std::string& id : ids) missing = missing || mail_find(id) < 0;
    if (missing && !mail_loaded_) {
        MailWait wait;
        wait.kind = MailWaitKind::BulkStatus;
        wait.ids = ids;
        wait.status = status;
        if (mail_start_load("the client marked mails", std::move(wait), now)) return;
    }
    mail_bulk_status(conn, ids, status);
}

void PlayerSession::register_mail_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerFetchMail", [](PlayerSession& s, const RpcContext& c) { s.on_fetch_mail(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerFetchMailNum", [](PlayerSession& s, const RpcContext& c) { s.on_fetch_mail_num(c.conn, c.rpc); });
    t.add(RpcScope::SocialComponent, "ServerGetMailData", [](PlayerSession& s, const RpcContext& c) { s.on_get_mail_data(c.conn, c.rpc, c.now); });
    t.add(RpcScope::SocialComponent, "ServerRefreshMailHeaders", [](PlayerSession& s, const RpcContext& c) { s.on_refresh_mail_headers(c.conn, c.rpc, c.now); });
    t.add(RpcScope::SocialComponent, "ServerClaimMailItem", [](PlayerSession& s, const RpcContext& c) { s.on_claim_mail_item(c.conn, c.rpc, c.now); });
    t.add(RpcScope::SocialComponent, "ServerClaimMailCurrency", [](PlayerSession& s, const RpcContext& c) { s.on_claim_mail_currency(c.conn, c.rpc); });
    t.add(RpcScope::SocialComponent, "ServerClaimAllMail", [](PlayerSession& s, const RpcContext& c) { s.on_claim_all_mail(c.conn, c.rpc, c.now); });
    t.add(RpcScope::SocialComponent, "ServerUpdateMailStatus", [](PlayerSession& s, const RpcContext& c) { s.on_update_mail_status(c.conn, c.rpc, c.now); });
    t.add(RpcScope::SocialComponent, "ServerBulkUpdateMailStatus", [](PlayerSession& s, const RpcContext& c) { s.on_bulk_update_mail_status(c.conn, c.rpc, c.now); });
}

PlayerSession::GmMailResult PlayerSession::gm_send_mail(Connection& conn, const std::string& sender, const std::string& subject, const std::string& body,
                                                        int64_t item_record, int32_t item_count, const std::string& by, GmMailDone when_done) {
    GmMailResult out;
    if (!sink_) {
        out.refusal = "the mail store is not available";
        return out;
    }
    if (finished_ || failed_) {
        out.refusal = "the session has ended";
        return out;
    }
    svc::NewMail mail;
    mail.sender = sender;
    mail.subject = subject;
    mail.body = body;
    mail.expires_in_ms = 0;
    if (item_record != 0) {
        if (item_count <= 0) {
            out.refusal = "an item needs a count above zero";
            return out;
        }
        int stack = 1;
        if (world_ && world_->content()) {
            std::string error;
            const std::optional<content::ItemRecord> record = world_->content()->item(item_record, error);
            if (!record) {
                out.refusal = "the item record is not in the content database";
                return out;
            }
            stack = std::max(1, record->max_stack_size);
            if ((item_count + stack - 1) / stack > 20) {
                out.refusal = "a mail carries at most 20 item stacks";
                return out;
            }
            std::vector<svc::InventoryItem> stacks;
            stacks_of(item_record, item_count, *record, stacks, 0);
            for (svc::InventoryItem& item : stacks) mail.items.push_back(svc::MailItem{std::move(item), false});
        } else {
            if (item_count > 20) {
                out.refusal = "a mail carries at most 20 item stacks";
                return out;
            }
            for (int32_t left = item_count; left > 0; --left) {
                svc::InventoryItem item;
                item.container = svc::ItemContainer::Backpack;
                item.item_record_id = item_record;
                item.quantity = 1;
                item.quality = 1;
                mail.items.push_back(svc::MailItem{std::move(item), false});
            }
        }
    }
    if (!world_) {
        out.refusal = "the mail store is not available";
        return out;
    }
    MailJobRequest request;
    request.kind = MailJobKind::CreateMail;
    request.character_id = character_id_;
    request.new_mail = std::move(mail);
    PendingMail job;
    job.kind = MailJobKind::CreateMail;
    job.gm_done = std::move(when_done);
    job.ticket = world_->submit_mail_job(std::move(request));
    out.pending = true;
    out.ticket = job.ticket;
    mail_pending_.push_back(std::move(job));
    (void)conn;   // (the answer is sent when the job lands, on the tick that runs run_mail)
    return out;
}

void PlayerSession::mail_create_landed(Connection& conn, PendingMail job, const svc::MailResult& made) {
    GmMailResult out;
    if (made.status != svc::CallStatus::Ok) {
        out.refusal = made.reason.empty() ? svc::call_status_name(made.status) : made.reason;
        spdlog::log(level_for(mail_refused_logged_), "[mail] {}: the admin send failed ({}: {})", name(), svc::call_status_name(made.status), made.reason);
    } else {
        out.ok = true;
        out.mail_id = made.mail.id;
        // The store has it.  A cache that was not read yet shows it after the next list read, so the count is kept here too.
        bool listed = false;
        for (const svc::Mail& mail : mails_) listed = listed || mail.id == made.mail.id;
        if (!listed) {
            mails_.insert(mails_.begin(), made.mail);
            if ((made.mail.status & (svc::kMailStatusDeleted | svc::kMailStatusRead)) == 0) ++mail_unread_;
        }
        mail_send_unread(conn, "the admin entry added a mail");
        mail_refresh("the admin entry added a mail");
        spdlog::log(level_for(mail_logged_), "[mail] {}: the admin entry added a mail ({} unread)", name(), mail_unread_);
    }
    out.unread = mail_unread_;
    if (job.gm_done) {
        job.gm_done(job.ticket, out);
    } else {
        mail_gm_done_[job.ticket] = out;
    }
}

bool PlayerSession::take_gm_mail(uint64_t ticket, GmMailResult& out) {
    const auto it = mail_gm_done_.find(ticket);
    if (it == mail_gm_done_.end()) return false;
    out = std::move(it->second);
    mail_gm_done_.erase(it);
    return true;
}

void PlayerSession::close_mail(const char* why) {
    (void)why;
    for (PendingMail& job : mail_pending_) {
        if (world_) world_->forget_mail_job(job.ticket);
        if (job.gm_done) {
            GmMailResult out;
            out.refusal = "the session has ended";
            job.gm_done(job.ticket, out);
        }
    }
    mail_pending_.clear();
    mail_claim_in_flight_.clear();
    mail_added_events_.clear();
    mail_gm_done_.clear();
}

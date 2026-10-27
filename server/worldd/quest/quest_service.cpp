// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "quest/quest_service.h"

#include "aoc/quest/v1/quest.grpc.pb.h"
#include "svc/grpc_services.h"
#include "svc/service_token.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace aoc::worldd::quest {

namespace questv1 = ::aoc::quest::v1;

struct GrpcQuestApi::Stubs {
    std::unique_ptr<questv1::QuestService::Stub> quest;
};

GrpcQuestApi::GrpcQuestApi(const std::string& address, std::string token, std::chrono::milliseconds timeout)
    : stubs_(std::make_unique<Stubs>()), token_(std::move(token)), timeout_(timeout) {
    grpc::ChannelArguments args;
    // come back quickly after the service restarts (gRPC's default backoff grows to 120 s)
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 200);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 200);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 5000);
    stubs_->quest = questv1::QuestService::NewStub(grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args));
}

GrpcQuestApi::~GrpcQuestApi() = default;

svc::QuestLogResult GrpcQuestApi::load_log(const std::string& character_id) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + timeout_);
    ctx.AddMetadata(svc::kAuthorizationMetadata, "Bearer " + token_);
    questv1::LoadLogRequest req;
    req.set_character_id(character_id);
    questv1::LoadLogResponse resp;
    svc::QuestLogResult r;
    r.status = svc::map_grpc_status(stubs_->quest->LoadLog(&ctx, req, &resp));
    if (r.status != svc::CallStatus::Ok) return r;
    for (const auto& q : resp.log().instances()) {
        svc::SavedQuest s;
        s.instance_guid = q.instance_guid();
        s.record_id = q.record_id();
        s.status = q.status();
        s.start_time = q.start_time();
        s.source_guid = q.source_guid();
        for (const auto& o : q.objectives()) {
            svc::SavedObjective so;
            so.instance_guid = o.instance_guid();
            so.record_id = o.record_id();
            so.active = o.active();
            so.awake = o.awake();
            so.hidden = o.hidden();
            so.completed = o.completed();
            so.succeeded = o.succeeded();
            so.failed = o.failed();
            so.count = o.count();
            so.start_time = o.start_time();
            s.objectives.push_back(std::move(so));
        }
        r.log.instances.push_back(std::move(s));
    }
    for (const auto& c : resp.log().completed()) r.log.completed.push_back({c.record_id(), c.count()});
    for (const auto& v : resp.log().variables()) r.log.variables.push_back({v.name(), v.value()});
    return r;
}

svc::CallStatus GrpcQuestApi::save_log(const std::string& character_id, const svc::SavedQuestLog& log) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + timeout_);
    ctx.AddMetadata(svc::kAuthorizationMetadata, "Bearer " + token_);
    questv1::SaveLogRequest req;
    req.set_character_id(character_id);
    questv1::QuestLog* out = req.mutable_log();
    for (const svc::SavedQuest& s : log.instances) {
        questv1::QuestInstance* q = out->add_instances();
        q->set_instance_guid(s.instance_guid);
        q->set_record_id(s.record_id);
        q->set_status(s.status);
        q->set_start_time(s.start_time);
        q->set_source_guid(s.source_guid);
        for (const svc::SavedObjective& so : s.objectives) {
            questv1::ObjectiveState* o = q->add_objectives();
            o->set_instance_guid(so.instance_guid);
            o->set_record_id(so.record_id);
            o->set_active(so.active);
            o->set_awake(so.awake);
            o->set_hidden(so.hidden);
            o->set_completed(so.completed);
            o->set_succeeded(so.succeeded);
            o->set_failed(so.failed);
            o->set_count(so.count);
            o->set_start_time(so.start_time);
        }
    }
    for (const svc::SavedCompleted& c : log.completed) {
        questv1::CompletedQuest* o = out->add_completed();
        o->set_record_id(c.record_id);
        o->set_count(c.count);
    }
    for (const svc::SavedVariable& v : log.variables) {
        questv1::PlayerVariable* o = out->add_variables();
        o->set_name(v.name);
        o->set_value(v.value);
    }
    questv1::SaveLogResponse resp;
    return svc::map_grpc_status(stubs_->quest->SaveLog(&ctx, req, &resp));
}

svc::CallStatus GrpcQuestApi::take_commands(const std::string& character_id, std::vector<svc::QuestCommand>& out) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + timeout_);
    ctx.AddMetadata(svc::kAuthorizationMetadata, "Bearer " + token_);
    questv1::TakeCommandsRequest req;
    req.set_character_id(character_id);
    questv1::TakeCommandsResponse resp;
    const svc::CallStatus st = svc::map_grpc_status(stubs_->quest->TakeCommands(&ctx, req, &resp));
    if (st != svc::CallStatus::Ok) return st;
    for (const auto& c : resp.commands()) out.push_back({c.id(), c.kind(), c.quest(), c.name(), c.value()});
    return st;
}

// ---- the worker ------------------------------------------------------------------------------------------------------------------------------------------------------

QuestWorker::QuestWorker(svc::QuestApi& api, Config config) : api_(api), config_(config), thread_([this] { run(); }) {}

QuestWorker::~QuestWorker() {
    flush(config_.destructor_wait);
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void QuestWorker::save(const std::string& character_id, svc::SavedQuestLog log) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = entries_.find(character_id);
        if (it == entries_.end()) {
            order_.push_back(character_id);
            entries_[character_id] = Entry{std::move(log), 0};
        } else {
            it->second = Entry{std::move(log), 0};   // the latest log wins
        }
    }
    cv_.notify_one();
}

void QuestWorker::watch(const std::string& character_id) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        watched_.insert(character_id);
    }
    cv_.notify_one();   // the thread sleeps while nobody is watched: it takes the first poll now
}

void QuestWorker::unwatch(const std::string& character_id) {
    std::lock_guard<std::mutex> lock(mu_);
    watched_.erase(character_id);
    inbox_.erase(character_id);
}

std::vector<svc::QuestCommand> QuestWorker::take(const std::string& character_id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = inbox_.find(character_id);
    if (it == inbox_.end()) return {};
    std::vector<svc::QuestCommand> out = std::move(it->second);
    inbox_.erase(it);
    return out;
}

bool QuestWorker::flush(std::chrono::milliseconds max_wait) {
    std::unique_lock<std::mutex> lock(mu_);
    return idle_cv_.wait_for(lock, max_wait, [this] { return entries_.empty() && !in_flight_; });
}

size_t QuestWorker::saved() const {
    std::lock_guard<std::mutex> lock(mu_);
    return saved_;
}

size_t QuestWorker::failed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return failed_;
}

void QuestWorker::run() {
    using clock = std::chrono::steady_clock;
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        // the next thing to do: a save that waits, or the commands' turn
        const auto now = clock::now();
        const bool poll_due = !watched_.empty() && now >= next_poll_;
        if (stop_ && order_.empty()) return;
        if (order_.empty() && !poll_due) {
            if (watched_.empty()) cv_.wait(lock, [this] { return stop_ || !order_.empty() || !watched_.empty(); });
            else cv_.wait_until(lock, next_poll_, [this] { return stop_ || !order_.empty(); });
            continue;
        }
        if (!order_.empty()) {
            const std::string id = order_.front();
            order_.pop_front();
            auto it = entries_.find(id);
            if (it == entries_.end()) continue;
            Entry entry = it->second;
            in_flight_ = true;
            lock.unlock();
            const svc::CallStatus st = api_.save_log(id, entry.log);
            lock.lock();
            in_flight_ = false;
            const auto cur = entries_.find(id);
            const bool newer = cur != entries_.end() && !(cur->second.log == entry.log);   // a newer log came in while this one was in flight
            if (st == svc::CallStatus::Ok) {
                ++saved_;
                if (cur != entries_.end() && !newer) entries_.erase(cur);
                else if (cur != entries_.end()) order_.push_back(id);
            } else if (st == svc::CallStatus::Unavailable && cur != entries_.end() && !newer && ++cur->second.attempts < config_.max_attempts) {
                const auto wait = config_.retry_delay * cur->second.attempts;
                spdlog::warn("[quests] saving the log of {} failed (the quest service is not answering): try {} of {} in {} ms", id, cur->second.attempts + 1, config_.max_attempts, wait.count());
                lock.unlock();
                std::this_thread::sleep_for(wait);
                lock.lock();
                order_.push_back(id);
            } else {
                ++failed_;
                spdlog::error("[quests] the log of {} was not saved ({}): the quest service {}", id, svc::call_status_name(st),
                              st == svc::CallStatus::Refused ? "refused it" : st == svc::CallStatus::NotFound ? "has no such character" : "is not reachable");
                if (cur != entries_.end() && !newer) entries_.erase(cur);
                else if (cur != entries_.end()) order_.push_back(id);
            }
            if (entries_.empty()) idle_cv_.notify_all();
            continue;
        }
        // the GM commands of the characters that play
        next_poll_ = now + config_.poll_interval;
        const std::vector<std::string> ids(watched_.begin(), watched_.end());
        in_flight_ = true;
        lock.unlock();
        std::vector<std::pair<std::string, std::vector<svc::QuestCommand>>> got;
        for (const std::string& id : ids) {
            std::vector<svc::QuestCommand> cmds;
            if (api_.take_commands(id, cmds) == svc::CallStatus::Ok && !cmds.empty()) got.emplace_back(id, std::move(cmds));
        }
        lock.lock();
        in_flight_ = false;
        for (auto& [id, cmds] : got) {
            if (watched_.count(id) == 0) continue;   // the character left meanwhile: its commands are lost with it (they were taken from the service)
            auto& box = inbox_[id];
            box.insert(box.end(), std::make_move_iterator(cmds.begin()), std::make_move_iterator(cmds.end()));
        }
        if (entries_.empty()) idle_cv_.notify_all();
    }
}

}  // namespace aoc::worldd::quest

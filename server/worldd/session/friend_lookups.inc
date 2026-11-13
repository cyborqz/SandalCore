// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  session/friend_lookups.inc
//
//  Social, step 7: the lookup worker of one world.  A friend call's character lookup (a name for the request and the name check, an id for the other side)
//  is the character service's, and that call may take seconds; the network thread only submits it and takes the result on a later tick, so one player's
//  lookup cannot hold every player's tick (review finding 6).  The jobs are answered in the order they were submitted; the results wait under the ticket
//  the session was given.  A job holds the sink it was submitted with, so a worker that outlives nothing asks nothing.
// ============================================================================
#pragma once

#include <spdlog/spdlog.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace aoc { namespace worldd {

/// One worker thread for a world's friend lookups (session/player_world.h).
class FriendLookups {
public:
    FriendLookups() : thread_([this] { run(); }) {}
    ~FriendLookups() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
            jobs_.clear();
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    FriendLookups(const FriendLookups&) = delete;
    FriendLookups& operator=(const FriendLookups&) = delete;

    /// Queues a lookup of `key` and returns its ticket.  `sink` is asked on the worker; null answers Failed.
    uint64_t submit(const std::string& key, bool by_name, PositionSink* sink) {
        std::lock_guard<std::mutex> lock(mu_);
        const uint64_t ticket = next_++;
        if (stop_) {
            done_[ticket] = svc::CharacterResult{};
            return ticket;
        }
        jobs_.push_back(Job{ticket, key, by_name, sink});
        cv_.notify_one();
        return ticket;
    }

    /// Takes the result of `ticket` when it is ready; false when it is not (or was never submitted).
    bool take(uint64_t ticket, svc::CharacterResult& out) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = done_.find(ticket);
        if (it == done_.end()) return false;
        out = std::move(it->second);
        done_.erase(it);
        return true;
    }

private:
    struct Job {
        uint64_t     ticket = 0;
        std::string  key;
        bool         by_name = true;
        PositionSink* sink = nullptr;
    };

    void run() {
        std::unique_lock<std::mutex> lock(mu_);
        for (;;) {
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            Job job = std::move(jobs_.front());
            jobs_.pop_front();
            lock.unlock();
            svc::CharacterResult result;
            if (job.sink) result = job.by_name ? job.sink->character_by_name(job.key) : job.sink->character_by_id(job.key);
            lock.lock();
            done_[job.ticket] = std::move(result);
        }
    }

    std::mutex                              mu_;
    std::condition_variable                 cv_;
    std::deque<Job>                         jobs_;
    std::map<uint64_t, svc::CharacterResult> done_;
    bool                                    stop_ = false;
    uint64_t                                next_ = 1;
    std::thread                             thread_;   // last: it starts once every other member is built
};

uint64_t PlayerWorld::submit_friend_lookup(const std::string& key, bool by_name) const {
    if (!friend_lookups_) friend_lookups_ = std::make_unique<FriendLookups>();
    return friend_lookups_->submit(key, by_name, sink_);
}

bool PlayerWorld::take_friend_lookup(uint64_t ticket, svc::CharacterResult& out) const {
    return friend_lookups_ && friend_lookups_->take(ticket, out);
}

PlayerWorld::~PlayerWorld() = default;

}}  // namespace aoc::worldd

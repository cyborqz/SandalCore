// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/quest_service.h  (quests milestone, slice 1, "take a quest")
//
//  worldd's side of the quest service (aoc.quest.v1.QuestService):
//
//    GrpcQuestApi   svc::QuestApi over gRPC, a call carries the service token and a deadline
//    QuestWorker    a thread that does what must not block the game loop: it saves each character's whole quest log (the latest save of a character wins, a save the service
//                   cannot take is tried again a few times) and it takes the GM commands queued for the characters that play, every moment, into an inbox the session reads
//
//  A session saves a log whenever it changes and reads its inbox every tick.  A world server with no quest service plays without quests (the login says so).
// ============================================================================
#pragma once

#include "svc/services.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::quest {

class GrpcQuestApi : public svc::QuestApi {
public:
    GrpcQuestApi(const std::string& address, std::string token, std::chrono::milliseconds timeout = std::chrono::seconds(5));
    ~GrpcQuestApi() override;

    svc::QuestLogResult load_log(const std::string& character_id) override;
    svc::CallStatus     save_log(const std::string& character_id, const svc::SavedQuestLog& log) override;
    svc::CallStatus     take_commands(const std::string& character_id, std::vector<svc::QuestCommand>& out) override;

private:
    struct Stubs;
    std::unique_ptr<Stubs> stubs_;
    std::string            token_;
    std::chrono::milliseconds timeout_;
};

class QuestWorker {
public:
    struct Config {
        int                       max_attempts = 3;                      // tries for a save the service answers Unavailable
        std::chrono::milliseconds retry_delay{1000};                     // before try n + 1: n times this
        std::chrono::milliseconds poll_interval{1500};                   // how often the queued commands of the watched characters are taken
        std::chrono::milliseconds destructor_wait{2000};                 // the destructor's own flush
    };

    explicit QuestWorker(svc::QuestApi& api, Config config = {});
    ~QuestWorker();
    QuestWorker(const QuestWorker&) = delete;
    QuestWorker& operator=(const QuestWorker&) = delete;

    /// Queues the character's whole log; a newer one for the same character replaces it while it waits.  Never blocks on the network.
    void save(const std::string& character_id, svc::SavedQuestLog log);
    /// The character plays: its queued commands are taken from now on / not any more.
    void watch(const std::string& character_id);
    void unwatch(const std::string& character_id);
    /// The commands taken for the character since the last call, oldest first.
    std::vector<svc::QuestCommand> take(const std::string& character_id);

    /// Waits until no save is queued or running, at most `max_wait`; true when none was.
    bool flush(std::chrono::milliseconds max_wait);
    size_t saved() const;
    size_t failed() const;

private:
    struct Entry {
        svc::SavedQuestLog log;
        int                attempts = 0;
    };
    void run();

    svc::QuestApi&       api_;
    const Config         config_;
    mutable std::mutex   mu_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::deque<std::string>                order_;
    std::unordered_map<std::string, Entry> entries_;
    std::set<std::string>                  watched_;
    std::unordered_map<std::string, std::vector<svc::QuestCommand>> inbox_;
    std::chrono::steady_clock::time_point  next_poll_{};
    bool   in_flight_ = false;
    bool   stop_ = false;
    size_t saved_ = 0, failed_ = 0;
    std::thread thread_;
};

}  // namespace aoc::worldd::quest

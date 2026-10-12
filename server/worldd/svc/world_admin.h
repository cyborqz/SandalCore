// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/world_admin.h  (the GM travel tools)
//
//  worldd's admin API: WorldAdminService (proto/internal/aoc/worldadmin/v1), served on AOC_WORLD_ADMIN_ADDR (default 127.0.0.1:50056).  API-FIRST: worldctl and the
//  admin app to come (a 3D viewer that controls the server) call the same methods.
//
//    * Calls carry the service token ("authorization: Bearer <token>", svc/service_token.h), as every internal call does; without it (or with another) the call is refused
//      with Unauthenticated and nothing else happens.  The comparison does not stop at the first different byte; the token is never logged.  Refusals are logged at most
//      once a second.
//    * A call arrives on a gRPC thread.  It is NOT applied there: it is put on the AdminQueue and applied on worldd's network thread, between two ticks of the world, by
//      AdminQueue::drain (main's loop), where the sessions live; no session state is touched off-thread.  The gRPC thread waits for the answer (call_timeout, 5 s) and a call
//      the network thread did not start by then ends with Unavailable and is never applied (a call that was started is waited for).
//    * The work itself is GmTravel (session/gm_travel.h); this file is the door: the token, the queue, the mapping of its answers to gRPC's.
// ============================================================================
#pragma once

#include "session/gm_travel.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace grpc { class Server; }

namespace aoc { namespace worldd { namespace svc {

/// Work for the network thread.  Any thread may run() a task and wait for it; the network thread calls drain().
class AdminQueue {
public:
    /// Puts `task` on the queue and waits until the network thread has run it.  False, with the task NOT run and never to be run, when the queue is closed or the network thread
    /// did not start it within `timeout`.  A task the network thread has started is waited for, however long it takes.
    bool run(std::function<void()> task, std::chrono::milliseconds timeout);

    /// Network thread: runs the tasks waiting, at most `max` of them; returns how many it ran.  A throwing task is logged and counts as run.
    size_t drain(size_t max = 32);

    /// Refuses every later run() and releases the callers still waiting (their tasks are never run).
    void close();
// C-O-A - Cyb

    size_t pending() const;
    /// Tasks run, tasks given up on (timed out or closed): for the tests.
    uint64_t ran() const { return ran_.load(); }
    uint64_t abandoned() const { return abandoned_.load(); }

private:
    struct Call {
        std::function<void()>   task;
        std::mutex              mu;
        std::condition_variable cv;
        bool                    started = false, done = false, cancelled = false;
    };
    mutable std::mutex                 mu_;
    std::deque<std::shared_ptr<Call>>  queue_;
    bool                               closed_ = false;
    std::atomic<uint64_t>              ran_{0}, abandoned_{0};
};

/// The gRPC server.  Not copyable; stop() (or the destructor) shuts it down.
class WorldAdminServer {
public:
    struct Options {
        std::string               address = "127.0.0.1:50056";   // AOC_WORLD_ADMIN_ADDR
        std::string               token;
        std::chrono::milliseconds call_timeout{5000};
    };

    /// `travel` and `queue` outlive the server.
    WorldAdminServer(GmTravel& travel, AdminQueue& queue, Options options);
    ~WorldAdminServer();
    WorldAdminServer(const WorldAdminServer&) = delete;
    WorldAdminServer& operator=(const WorldAdminServer&) = delete;

    /// Binds and starts serving; false with `error` when the address cannot be bound.  Once.
    bool start(std::string* error);
    /// Stops serving (calls in flight end) and closes the queue.  Safe to call twice.
    void stop();
    /// The port bound (0 before start).
    int port() const { return port_; }
    /// Calls refused for the token, for the tests.
    uint64_t refused() const;

private:
    struct Service;
    GmTravel&                    travel_;
    AdminQueue&                  queue_;
    Options                      options_;
    std::unique_ptr<Service>     service_;
    std::unique_ptr<grpc::Server> server_;
    int                          port_ = 0;
};

}}}  // namespace aoc::worldd::svc

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/city_node_feed.h  (Nodes 1, stage 1)
//
//  worldd's view of the citynode service (services/internal/citynode, proto/internal/aoc/citynode/v1): a thread that calls WatchNodes, applies the
//  snapshot it sends first to the node registry (world/city_nodes.h) as one change, then each change after it, and calls again when the stream ends.
//
//    * Login never waits for it.  The thread is started after the registry is made and the seed's levels are loaded; whatever happens to the stream,
//      the registry keeps its last levels.
//    * When the service cannot be reached, ONE warning is logged for the outage ("... worldd runs with the seed's levels, retrying") and the thread
//      retries with a backoff (0.5 s growing to 5 s).  When a stream comes back its snapshot replaces every level at once, and a later outage warns once again.
//    * The stream's contract (citynode.proto): one snapshot (seq = the last change it includes), then changes numbered seq + 1, + 2 ... with no gap.  A
//      number that is not the next one is a desynchronisation: the stream is dropped and called again, whose snapshot is the whole state.  The service ends a
//      stream with Aborted (the reader fell too far behind) or Unavailable (it shuts down); both mean "call again".
//    * Calls carry the service token ("authorization: Bearer <token>", svc/service_token.h), as every internal call does.
// ============================================================================
#pragma once

#include "world/city_nodes.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace grpc { class ClientContext; }

namespace aoc { namespace worldd { namespace svc {

class CityNodeFeed {
public:
    struct Options {
        std::string address = "127.0.0.1:50055";   // AOC_CITYNODE_GRPC_ADDR
        std::string token;
        std::chrono::milliseconds initial_backoff{500};
        std::chrono::milliseconds max_backoff{5000};
    };

    /// `nodes` outlives the feed.
    CityNodeFeed(world::CityNodes& nodes, Options options);
    ~CityNodeFeed();
    CityNodeFeed(const CityNodeFeed&) = delete;
    CityNodeFeed& operator=(const CityNodeFeed&) = delete;

    void start();
    /// Cancels the stream in flight and joins the thread.  Safe to call twice.
    void stop();
// C.Y.B and C.O.A

    /// A stream is up and has delivered its snapshot.
    bool connected() const { return connected_.load(); }
    /// The seq of the last snapshot or change applied (0 before the first).
    uint64_t seq() const { return seq_.load(); }
    /// How many snapshots and changes were applied, how many warnings were logged, how many times a stream was (re)started: for the tests.
    uint64_t snapshots() const { return snapshots_.load(); }
    uint64_t changes() const { return changes_.load(); }
    uint64_t warnings() const { return warnings_.load(); }
    uint64_t attempts() const { return attempts_.load(); }

private:
    struct Impl;
    void run();
    bool sleep_unless_stopped(std::chrono::milliseconds d);

    world::CityNodes&        nodes_;
    Options                  options_;
    std::unique_ptr<Impl>    impl_;
    std::thread              thread_;
    std::mutex               mu_;
    std::condition_variable  cv_;
    bool                     stop_ = false;
    grpc::ClientContext*     active_ = nullptr;   // the call in flight, for stop() to cancel; guarded by mu_
    std::atomic<bool>        connected_{false};
    std::atomic<uint64_t>    seq_{0}, snapshots_{0}, changes_{0}, warnings_{0}, attempts_{0};
};

}}}  // namespace aoc::worldd::svc

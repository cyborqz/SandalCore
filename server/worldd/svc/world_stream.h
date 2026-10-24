// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/world_stream.h  (the world stream: worldd's read-only view of the world for admin tools)
//
//  WorldStreamService (proto/internal/aoc/worldadmin/v1), served on AOC_WORLD_STREAM_ADDR (default 127.0.0.1:50057) by a gRPC server of its own, beside the admin API
//  (svc/world_admin.h): a stream that holds a thread cannot starve the admin calls.  services/cmd/worldctl `watch` is the reference consumer.
//
//    * Calls carry the service token ("authorization: Bearer <token>", svc/service_token.h), as every internal call does; without it (or with another) the call is refused with
//      Unauthenticated and nothing else happens.  The comparison does not stop at the first different byte; the token is never logged.
//    * Publishing.  The network thread calls publish() in every pass of its loop; five times a second it copies a compact snapshot of the world (world/map_stream.h) into a double
//      buffer and wakes the streams.  A stream's gRPC thread works out what its subscriber is owed from the newest snapshot (world::mapstream::Cursor) and writes it.  worldd never
//      waits for a subscriber: a thread blocked in a write holds only itself, and at most Options::max_subscribers streams are open.
//    * Inspect and Search read the live world, so they are put on the AdminQueue and run on the network thread, between two ticks, like an admin call.  GetStatus reads the newest
//      snapshot and takes no hop.
//    * The cost of a publish is measured on every one and logged once a minute (the average, the worst, the sizes), and GetStatus carries it.
//
//  The implementation is a header of its own (svc/world_stream_impl.h), included by exactly one .cpp: svc/world_admin.cpp.
// ============================================================================
#pragma once

#include "svc/world_admin.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace grpc { class Server; }

namespace aoc { namespace worldd { namespace svc {

class WorldStreamServer {
public:
    /// The players of the world now (Endpoint::players), the same function the GM travel tools use.  Called on the network thread only.
    using Directory = std::function<std::vector<PlayerHandle>()>;

    struct Options {
        std::string               address = "127.0.0.1:50057";   // AOC_WORLD_STREAM_ADDR
        std::string               token;
        std::chrono::milliseconds call_timeout{5000};            // Inspect and Search: how long the network thread has to take the call
        size_t                    max_subscribers = 8;           // Subscribe streams open at once; one more is refused with ResourceExhausted
    };

    /// `world`, `queue` and whatever `directory` refers to outlive the server.
    WorldStreamServer(PlayerWorld& world, Directory directory, AdminQueue& queue, Options options);
    ~WorldStreamServer();
    WorldStreamServer(const WorldStreamServer&) = delete;
    WorldStreamServer& operator=(const WorldStreamServer&) = delete;

    /// Binds and starts serving; false with `error` when the address cannot be bound.  Once.
    bool start(std::string* error);
    /// Stops serving (streams end, calls in flight end) and closes the queue.  Safe to call twice.
    void stop();

    /// Network thread: call it in every pass of the loop; it publishes when 200 ms have passed since the last time.  It never throws.
    void publish();
    /// Network thread: how long the last pass of the loop worked (not counting the wait for a datagram), for the stream's tick_ms.
    void note_loop(std::chrono::microseconds busy);

    /// The port bound (0 before start).
    int port() const { return port_; }
    /// Streams open now.
    size_t subscribers() const;
    /// Calls refused for the token.
    uint64_t refused() const;

private:
    struct Impl;
    std::unique_ptr<Impl>         impl_;
    std::unique_ptr<grpc::Server> server_;
    int                           port_ = 0;
};

}}}  // namespace aoc::worldd::svc

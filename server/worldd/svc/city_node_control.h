// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/city_node_control.h  (Nodes 1, stage 2)
//
//  world::NodeControl over gRPC: the client's GM console reaches the citynode service (services/internal/citynode, proto/internal/aoc/citynode/v1) through the SAME methods
//  nodectl and the admin app use.  A call is queued to one worker thread (the network thread never waits for the service), carries the service token, and ends in the log:
//
//      [gm] ServerSetNodeLevel by Cyb: Joeva (Verra_RVR_Joeva) level 3 (Village), 0 XP, 0 %, lord none (changed)
//      [gm] ServerAddNodeExperience by Cyb: 1,200 XP added: Halcyon (Verra_RVR_Halcyon) level 3 ...
//      [gm] ServerGMGetNodeData by Cyb: Joeva (Verra_RVR_Joeva) level 4 (Town), 0 XP, 0 %, lord none
//      [gm] ... refused: <the service's reason>        (NotFound, InvalidArgument), or "the node service is unavailable" when it cannot be reached
//
//  A change comes back through CityNodeFeed to the registry and from there to every client's GameState; this class only asks.  The queue holds at most 64 calls (the
//  oldest is dropped, said once).
// ============================================================================
#pragma once

#include "world/node_control.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace aoc { namespace worldd { namespace svc {

class GrpcNodeControl : public world::NodeControl {
public:
    struct Options {
        std::string address = "127.0.0.1:50055";   // AOC_CITYNODE_GRPC_ADDR
        std::string token;
        int         deadline_ms = 3000;
    };
    /// Called with each line the worker logs (for the tests); the log gets it as well.
    using Report = std::function<void(const std::string& line)>;
// ~ C O A & C-Y-B ~

    explicit GrpcNodeControl(Options options, Report report = {});
    ~GrpcNodeControl() override;
    GrpcNodeControl(const GrpcNodeControl&) = delete;
    GrpcNodeControl& operator=(const GrpcNodeControl&) = delete;

    void set_level(const std::string& node, int level, bool disable_vassalship, const std::string& by) override;
    void add_experience(const std::string& node, int64_t amount, const std::string& by) override;
    void node_data(const std::string& node, const std::string& by) override;
    void clock(const std::string& by) override;
    void advance_clock(int64_t seconds, const std::string& by) override;

    /// Blocks until every queued call has finished (the tests).
    void drain();
    uint64_t calls_made() const;

private:
    struct Impl;
    void post(std::function<void()> job);
    void run();
    void say(const std::string& line);

    Options                  options_;
    Report                   report_;
    std::unique_ptr<Impl>    impl_;
    std::thread              thread_;
    mutable std::mutex       mu_;
    std::condition_variable  cv_, idle_cv_;
    std::deque<std::function<void()>> queue_;
    bool                     stop_ = false, busy_ = false, overflow_said_ = false;
    uint64_t                 calls_ = 0;
};

}}}  // namespace aoc::worldd::svc

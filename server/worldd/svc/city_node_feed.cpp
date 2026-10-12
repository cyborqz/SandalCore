// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/city_node_feed.cpp (see city_node_feed.h)
// ============================================================================
#include "svc/city_node_feed.h"

#include "aoc/citynode/v1/citynode.grpc.pb.h"
#include "svc/service_token.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace aoc { namespace worldd { namespace svc {

namespace nodev1 = ::aoc::citynode::v1;

struct CityNodeFeed::Impl {
    std::unique_ptr<nodev1::CityNodeService::Stub> stub;
};

namespace {

std::shared_ptr<grpc::Channel> make_channel(const std::string& address) {
    grpc::ChannelArguments args;
    // come back quickly after the service restarts (gRPC's default backoff grows to 120 s)
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 200);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 200);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 5000);
    return grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);
}

world::LiveNode live_of(const nodev1::Node& n) {
    world::LiveNode l;
    l.guid = n.guid();
    l.level = static_cast<int>(std::min<uint32_t>(n.level(), 1000));
    l.experience = n.experience();
    l.parent_guid = n.parent_guid();
    return l;
}

}  // namespace

CityNodeFeed::CityNodeFeed(world::CityNodes& nodes, Options options) : nodes_(nodes), options_(std::move(options)), impl_(std::make_unique<Impl>()) {
    impl_->stub = nodev1::CityNodeService::NewStub(make_channel(options_.address));
}
// Cyb and C O A

CityNodeFeed::~CityNodeFeed() { stop(); }

void CityNodeFeed::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void CityNodeFeed::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        if (active_) active_->TryCancel();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool CityNodeFeed::sleep_unless_stopped(std::chrono::milliseconds d) {
    std::unique_lock<std::mutex> lock(mu_);
    return !cv_.wait_for(lock, d, [this] { return stop_; });
}

void CityNodeFeed::run() {
    auto backoff = options_.initial_backoff;
    bool warned = false;   // one warning per outage
    while (true) {
        grpc::ClientContext ctx;
        ctx.AddMetadata(kAuthorizationMetadata, "Bearer " + options_.token);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            active_ = &ctx;
        }
        ++attempts_;
        nodev1::WatchNodesRequest req;
        std::unique_ptr<grpc::ClientReader<nodev1::WatchNodesResponse>> reader = impl_->stub->WatchNodes(&ctx, req);
        nodev1::WatchNodesResponse msg;
        bool got_snapshot = false, desync = false;
        uint64_t last = 0;
        while (reader->Read(&msg)) {
            switch (msg.event_case()) {
                case nodev1::WatchNodesResponse::kSnapshot: {
                    std::vector<world::LiveNode> all;
                    all.reserve(static_cast<size_t>(msg.snapshot().nodes_size()));
                    for (const nodev1::Node& n : msg.snapshot().nodes()) all.push_back(live_of(n));
                    nodes_.apply_snapshot(all, world::LevelSource::Service);
                    last = msg.seq();
                    seq_.store(last);
                    ++snapshots_;
                    got_snapshot = true;
                    connected_.store(true);
                    backoff = options_.initial_backoff;
                    spdlog::info("[nodes] citynode at {}: the whole state ({} nodes, seq {}): {}", options_.address, all.size(), last, nodes_.describe_levels());
                    warned = false;
                    break;
                }
                case nodev1::WatchNodesResponse::kNodeChanged: {
                    if (!got_snapshot || msg.seq() != last + 1) {
                        desync = true;
                        break;
                    }
                    last = msg.seq();
                    seq_.store(last);
                    const nodev1::NodeChange& c = msg.node_changed();
                    if (nodes_.apply_change(live_of(c.node()))) {
                        ++changes_;
                        spdlog::info("[nodes] {} is now level {} (was {}, seq {})", c.node().display_name(), c.node().level(), c.previous_level(), last);
                    }
                    break;
                }
                case nodev1::WatchNodesResponse::kClockChanged:
                    if (!got_snapshot || msg.seq() != last + 1) {
                        desync = true;
                        break;
                    }
                    last = msg.seq();
                    seq_.store(last);
                    break;
                case nodev1::WatchNodesResponse::EVENT_NOT_SET: break;
            }
            if (desync) {
                ++warnings_;
                warned = true;   // this is the outage's warning
                spdlog::warn("[nodes] citynode sent seq {} after {}: calling WatchNodes again for the whole state", msg.seq(), last);
                ctx.TryCancel();
                break;
            }
        }
        const grpc::Status status = reader->Finish();
        connected_.store(false);
        {
            std::lock_guard<std::mutex> lock(mu_);
            active_ = nullptr;
            if (stop_) return;
        }
        if (!warned) {
            ++warnings_;
            warned = true;
            if (got_snapshot) {
                spdlog::warn("[nodes] the citynode stream at {} ended ({}): worldd keeps the levels it has and calls again", options_.address, status.error_message());
            } else {
                spdlog::warn("[nodes] citynode at {} is not reachable ({}): worldd runs with the levels of the {} ({}) and keeps retrying",
                             options_.address, status.error_message().empty() ? "the stream did not start" : status.error_message(),
                             world::level_source_name(nodes_.source()), nodes_.describe_levels());
            }
        }
        if (!sleep_unless_stopped(backoff)) return;
        backoff = std::min(backoff * 2, options_.max_backoff);
    }
}

}}}  // namespace aoc::worldd::svc

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/city_node_control.cpp (see city_node_control.h)
// ============================================================================
#include "svc/city_node_control.h"

#include "aoc/citynode/v1/citynode.grpc.pb.h"
#include "svc/service_token.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>

namespace aoc { namespace worldd { namespace svc {

namespace nodev1 = ::aoc::citynode::v1;

struct GrpcNodeControl::Impl {
    std::unique_ptr<nodev1::CityNodeService::Stub> stub;
};

namespace {

constexpr size_t kMaxQueued = 64;

std::string with_commas(int64_t v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return v < 0 ? "-" + s : s;
}

std::string describe(const nodev1::Node& n) {
    char buf[320];
    std::snprintf(buf, sizeof buf, "%s (%s) level %u (%s), %s XP, %u %%, lord %s", n.display_name().c_str(), n.name().c_str(), n.level(), n.level_name().c_str(),
                  with_commas(n.experience()).c_str(), n.percent(), n.parent_guid() == 0 ? "none" : std::to_string(n.parent_guid()).c_str());
    return buf;
}

std::string refusal(const grpc::Status& st) {
    switch (st.error_code()) {
        case grpc::StatusCode::UNAVAILABLE:
        case grpc::StatusCode::DEADLINE_EXCEEDED: return "the node service is unavailable";
        case grpc::StatusCode::NOT_FOUND: return "refused: " + st.error_message();
        case grpc::StatusCode::INVALID_ARGUMENT: return "refused: " + st.error_message();
        default: return "failed (" + std::to_string(static_cast<int>(st.error_code())) + "): " + st.error_message();
    }
}

void prepare(grpc::ClientContext& ctx, int deadline_ms, const std::string& token) {
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_ms));
    ctx.AddMetadata(kAuthorizationMetadata, "Bearer " + token);
}

}  // namespace
// -- C.Y.B . CoA --

GrpcNodeControl::GrpcNodeControl(Options options, Report report) : options_(std::move(options)), report_(std::move(report)), impl_(std::make_unique<Impl>()) {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 200);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 5000);
    impl_->stub = nodev1::CityNodeService::NewStub(grpc::CreateCustomChannel(options_.address, grpc::InsecureChannelCredentials(), args));
    thread_ = std::thread([this] { run(); });
}

GrpcNodeControl::~GrpcNodeControl() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void GrpcNodeControl::say(const std::string& line) {
    spdlog::info("[gm] {}", line);
    if (report_) report_(line);
}

void GrpcNodeControl::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (queue_.size() >= kMaxQueued) {
            queue_.pop_front();
            if (!overflow_said_) {
                overflow_said_ = true;
                spdlog::warn("[gm] the node GM calls are queued faster than the node service answers: the oldest is dropped (said once)");
            }
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void GrpcNodeControl::run() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;   // worldd is stopping: what is still queued is dropped, not waited for
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        job();
        {
            std::lock_guard<std::mutex> lock(mu_);
            busy_ = false;
            ++calls_;
        }
        idle_cv_.notify_all();
    }
}

void GrpcNodeControl::drain() {
    std::unique_lock<std::mutex> lock(mu_);
    idle_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

uint64_t GrpcNodeControl::calls_made() const {
    std::lock_guard<std::mutex> lock(mu_);
    return calls_;
}

void GrpcNodeControl::set_level(const std::string& node, int level, bool disable_vassalship, const std::string& by) {
    post([=, this] {
        grpc::ClientContext ctx;
        prepare(ctx, options_.deadline_ms, options_.token);
        nodev1::SetNodeLevelRequest req;
        req.mutable_node()->set_name(node);
        req.set_level(level);
        req.set_disable_vassalship(disable_vassalship);
        nodev1::SetNodeLevelResponse resp;
        const grpc::Status st = impl_->stub->SetNodeLevel(&ctx, req, &resp);
        if (!st.ok()) return say("ServerSetNodeLevel by " + by + ": \"" + node + "\" to level " + std::to_string(level) + " " + refusal(st));
        say("ServerSetNodeLevel by " + by + ": " + describe(resp.node()) + (resp.changed() ? " (changed)" : " (it already had that level)"));
    });
}

void GrpcNodeControl::add_experience(const std::string& node, int64_t amount, const std::string& by) {
    post([=, this] {
        grpc::ClientContext ctx;
        prepare(ctx, options_.deadline_ms, options_.token);
        nodev1::AddNodeExperienceRequest req;
        req.mutable_node()->set_name(node);
        req.set_amount(amount);
        nodev1::AddNodeExperienceResponse resp;
        const grpc::Status st = impl_->stub->AddNodeExperience(&ctx, req, &resp);
        if (!st.ok()) return say("ServerAddNodeExperience by " + by + ": \"" + node + "\" " + with_commas(amount) + " XP " + refusal(st));
        say("ServerAddNodeExperience by " + by + ": " + with_commas(amount) + " XP added: " + describe(resp.node()));
    });
}
// -- C Y B x C-O-A --

void GrpcNodeControl::node_data(const std::string& node, const std::string& by) {
    post([=, this] {
        grpc::ClientContext ctx;
        prepare(ctx, options_.deadline_ms, options_.token);
        nodev1::GetNodeRequest req;
        req.mutable_node()->set_name(node);
        nodev1::GetNodeResponse resp;
        const grpc::Status st = impl_->stub->GetNode(&ctx, req, &resp);
        if (!st.ok()) return say("ServerGMGetNodeData by " + by + ": \"" + node + "\" " + refusal(st));
        say("ServerGMGetNodeData by " + by + ": " + describe(resp.node()));
    });
}

void GrpcNodeControl::clock(const std::string& by) {
    post([=, this] {
        grpc::ClientContext ctx;
        prepare(ctx, options_.deadline_ms, options_.token);
        nodev1::GetClockRequest req;
        nodev1::GetClockResponse resp;
        const grpc::Status st = impl_->stub->GetClock(&ctx, req, &resp);
        if (!st.ok()) return say("ServerGMWhatTimeIsIt by " + by + ": " + refusal(st));
        const auto& c = resp.clock();
        say("ServerGMWhatTimeIsIt by " + by + ": node time " + std::to_string(c.node_unix_ms() / 1000) + " (unix), speed " + std::to_string(c.speed()) + ", offset " +
            std::to_string(c.offset_seconds()) + " s");
    });
}

void GrpcNodeControl::advance_clock(int64_t seconds, const std::string& by) {
    post([=, this] {
        grpc::ClientContext ctx;
        prepare(ctx, options_.deadline_ms, options_.token);
        nodev1::AdvanceClockRequest req;
        req.set_seconds(seconds);
        nodev1::AdvanceClockResponse resp;
        const grpc::Status st = impl_->stub->AdvanceClock(&ctx, req, &resp);
        if (!st.ok()) return say("ServerGMFastForwardRealTime by " + by + ": " + std::to_string(seconds) + " s " + refusal(st));
        say("ServerGMFastForwardRealTime by " + by + ": the node clock skipped " + std::to_string(seconds) + " s");
    });
}

}}}  // namespace aoc::worldd::svc

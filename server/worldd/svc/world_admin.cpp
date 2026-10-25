// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/world_admin.cpp (see world_admin.h)
// ============================================================================
#include "svc/world_admin.h"

#include "aoc/worldadmin/v1/worldadmin.grpc.pb.h"
#include "svc/service_token.h"
#include "svc/world_stream_impl.h"   // the world stream's server (svc/world_stream.h) is built in this file: its implementation is a header of its own, included here only

#include <grpcpp/grpcpp.h>
#include <grpcpp/resource_quota.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>

namespace aoc { namespace worldd { namespace svc {

namespace adminv1 = ::aoc::worldadmin::v1;

// ---- the queue ------------------------------------------------------------------------------------------------------------------------------------

bool AdminQueue::run(std::function<void()> task, std::chrono::milliseconds timeout) {
    auto call = std::make_shared<Call>();
    call->task = std::move(task);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) {
            ++abandoned_;
            return false;
        }
        queue_.push_back(call);
    }
    std::unique_lock<std::mutex> lock(call->mu);
    if (!call->cv.wait_for(lock, timeout, [&] { return call->started || call->cancelled; })) {
        call->cancelled = true;   // the network thread has not started it: it never will
        ++abandoned_;
        return false;
    }
    if (!call->started) {   // closed while it waited
        ++abandoned_;
        return false;
    }
    call->cv.wait(lock, [&] { return call->done; });
    return true;
}

size_t AdminQueue::drain(size_t max) {
    size_t ran = 0;
    while (ran < max) {
        std::shared_ptr<Call> call;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (queue_.empty()) break;
            call = std::move(queue_.front());
            queue_.pop_front();
        }
        {
            std::lock_guard<std::mutex> lock(call->mu);
            if (call->cancelled) continue;   // its caller gave up
            call->started = true;
        }
        call->cv.notify_all();
        try {
            call->task();
        } catch (const std::exception& e) {
            spdlog::error("[admin] a call threw {}: it is answered as if it had run", e.what());
        } catch (...) {
            spdlog::error("[admin] a call threw: it is answered as if it had run");
        }
        {
            std::lock_guard<std::mutex> lock(call->mu);
            call->done = true;
        }
        call->cv.notify_all();
        ++ran;
        ++ran_;
    }
    return ran;
}
// Cyb / C.O.A

void AdminQueue::close() {
    std::deque<std::shared_ptr<Call>> waiting;
    {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        waiting.swap(queue_);
    }
    for (const auto& call : waiting) {
        {
            std::lock_guard<std::mutex> lock(call->mu);
            call->cancelled = true;
        }
        call->cv.notify_all();
    }
}

size_t AdminQueue::pending() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

// ---- the service ----------------------------------------------------------------------------------------------------------------------------------

namespace {

grpc::StatusCode code_of(GmStatus s) {
    switch (s) {
        case GmStatus::Ok: return grpc::StatusCode::OK;
        case GmStatus::NotFound: return grpc::StatusCode::NOT_FOUND;
        case GmStatus::InvalidArgument: return grpc::StatusCode::INVALID_ARGUMENT;
        case GmStatus::FailedPrecondition: return grpc::StatusCode::FAILED_PRECONDITION;
    }
    return grpc::StatusCode::INTERNAL;
}

void put(adminv1::Position* out, const GmPosition& p) {
    out->set_x(p.x);
    out->set_y(p.y);
    out->set_z(p.z);
}

}  // namespace

struct WorldAdminServer::Service final : adminv1::WorldAdminService::Service {
    explicit Service(WorldAdminServer& o) : owner(o) {}
    WorldAdminServer&     owner;
    std::atomic<uint64_t> refused_calls{0};
    std::mutex            log_mu;
    std::chrono::steady_clock::time_point last_log{};
    uint64_t              held_back = 0;
// built by C:Y:B with C.O.A

    /// The token check: exactly one "authorization" header, equal to "Bearer <token>", compared without stopping at the first different byte.
    grpc::Status authorize(grpc::ServerContext* ctx, const char* method) {
        const auto range = ctx->client_metadata().equal_range(kAuthorizationMetadata);
        bool ok = false;
        if (!owner.options_.token.empty() && std::distance(range.first, range.second) == 1) {
            const std::string want = "Bearer " + owner.options_.token;
            const grpc::string_ref got = range.first->second;
            unsigned char diff = got.size() == want.size() ? 0 : 1;
            for (size_t i = 0; i < want.size(); ++i) diff |= static_cast<unsigned char>(i < got.size() ? got.data()[i] : 0) ^ static_cast<unsigned char>(want[i]);
            ok = diff == 0;
        }
        if (ok) return grpc::Status::OK;
        ++refused_calls;
        {
            std::lock_guard<std::mutex> lock(log_mu);
            ++held_back;
            const auto now = std::chrono::steady_clock::now();
            if (last_log == std::chrono::steady_clock::time_point{} || now - last_log >= std::chrono::seconds(1)) {
                spdlog::warn("[admin] {} from {} refused: missing or wrong service token ({} refusal(s) since the last line)", method, ctx->peer(), held_back);
                held_back = 0;
                last_log = now;
            }
        }
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "missing or wrong service token");
    }

    /// Runs `f` on the network thread and waits for it.
    template <class F>
    grpc::Status on_net_thread(F&& f) {
        if (!owner.queue_.run(std::forward<F>(f), owner.options_.call_timeout)) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "worldd's network thread did not take the call in time (is worldd stopping?)");
        }
        return grpc::Status::OK;
    }

    grpc::Status ListPlayers(grpc::ServerContext* ctx, const adminv1::ListPlayersRequest*, adminv1::ListPlayersResponse* resp) override {
        if (auto s = authorize(ctx, "ListPlayers"); !s.ok()) return s;
        std::vector<GmPlayerInfo> players;
        if (auto s = on_net_thread([&] { players = owner.travel_.list_players(); }); !s.ok()) return s;
        for (const GmPlayerInfo& p : players) {
            adminv1::Player* out = resp->add_players();
            out->set_name(p.name);
            out->set_character_id(p.character_id);
            put(out->mutable_position(), p.position);
            out->set_yaw_degrees(p.yaw_degrees);
            out->set_mounted(p.mounted);
            out->set_dead(p.dead);
            out->set_move_speed_multiplier(p.move_speed_multiplier);
        }
        return grpc::Status::OK;
    }

    grpc::Status ListPlaces(grpc::ServerContext* ctx, const adminv1::ListPlacesRequest*, adminv1::ListPlacesResponse* resp) override {
        if (auto s = authorize(ctx, "ListPlaces"); !s.ok()) return s;
        std::vector<world::Place> places;
        if (auto s = on_net_thread([&] { places = owner.travel_.list_places(); }); !s.ok()) return s;
        for (const world::Place& p : places) {
            adminv1::Place* out = resp->add_places();
            out->set_name(p.name);
            out->set_display_name(p.display_name);
            out->set_kind(p.kind);
            put(out->mutable_position(), GmPosition{p.x, p.y, p.z});
        }
        return grpc::Status::OK;
    }

    grpc::Status Teleport(grpc::ServerContext* ctx, const adminv1::TeleportRequest* req, adminv1::TeleportResponse* resp) override {
        if (auto s = authorize(ctx, "Teleport"); !s.ok()) return s;
        GmTeleportRequest r;
        r.character = req->character();
        switch (req->target_case()) {
            case adminv1::TeleportRequest::kPlace:
                r.place = req->place();
                if (r.place.empty()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "the place is empty");
                break;
            case adminv1::TeleportRequest::kPoint:
                r.x = req->point().x();
                r.y = req->point().y();
                r.z = req->point().z();
                r.has_z = req->point().has_z();
                break;
            case adminv1::TeleportRequest::TARGET_NOT_SET:
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "give a place or a point");
        }
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmTeleportResult out;
        if (auto s = on_net_thread([&] { out = owner.travel_.teleport(r, by); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_character(out.character);
        put(resp->mutable_from(), out.from);
        put(resp->mutable_to(), out.to);
        resp->set_place(out.place);
        resp->set_dismounted(out.dismounted);
        resp->set_ground_height(out.ground_height);
        return grpc::Status::OK;
    }

    grpc::Status SetMoveSpeed(grpc::ServerContext* ctx, const adminv1::SetMoveSpeedRequest* req, adminv1::SetMoveSpeedResponse* resp) override {
        if (auto s = authorize(ctx, "SetMoveSpeed"); !s.ok()) return s;
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmSpeedResult out;
        if (auto s = on_net_thread([&] { out = owner.travel_.set_speed(req->character(), req->multiplier(), by); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_character(out.character);
        resp->set_previous(out.previous);
        resp->set_multiplier(out.multiplier);
        return grpc::Status::OK;
    }

    grpc::Status GiveMoney(grpc::ServerContext* ctx, const adminv1::GiveMoneyRequest* req, adminv1::GiveMoneyResponse* resp) override {
        if (auto s = authorize(ctx, "GiveMoney"); !s.ok()) return s;
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmMoneyGrant out;
        if (auto s = on_net_thread([&] { out = owner.travel_.give_money(req->character(), req->copper(), by); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_character(out.character);
        resp->set_previous(out.previous);
        resp->set_balance(out.balance);
        resp->set_added(out.added);
        return grpc::Status::OK;
    }

    grpc::Status GiveItem(grpc::ServerContext* ctx, const adminv1::GiveItemRequest* req, adminv1::GiveItemResponse* resp) override {
        if (auto s = authorize(ctx, "GiveItem"); !s.ok()) return s;
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmItemGrant out;
        if (auto s = on_net_thread([&] { out = owner.travel_.give_item(req->character(), req->item(), req->count() == 0 ? 1 : req->count(), by, req->rarity(), req->quality()); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_character(out.character);
        resp->set_item(out.item);
        resp->set_item_guid(out.item_guid);
        resp->set_count(out.count);
        resp->set_container(out.container);
        resp->set_quality(out.quality);
        resp->set_rarity(out.rarity);
        return grpc::Status::OK;
    }

    grpc::Status FinishJobs(grpc::ServerContext* ctx, const adminv1::FinishJobsRequest* req, adminv1::FinishJobsResponse* resp) override {
        if (auto s = authorize(ctx, "FinishJobs"); !s.ok()) return s;
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmFinishResult out;
        if (auto s = on_net_thread([&] { out = owner.travel_.finish_jobs(req->character(), req->station(), by); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_character(out.character);
        resp->set_finished(static_cast<int32_t>(out.finished));
        return grpc::Status::OK;
    }

    grpc::Status TeachRecipe(grpc::ServerContext* ctx, const adminv1::TeachRecipeRequest* req, adminv1::TeachRecipeResponse* resp) override {
        if (auto s = authorize(ctx, "TeachRecipe"); !s.ok()) return s;
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmRecipeResult out;
        if (auto s = on_net_thread([&] { out = owner.travel_.teach_recipe(req->character(), req->recipe(), by); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_character(out.character);
        resp->set_recipe(out.recipe);
        resp->set_recipe_guid(out.recipe_guid);
        resp->set_taught(out.taught);
        return grpc::Status::OK;
    }

    grpc::Status ForgetRecipe(grpc::ServerContext* ctx, const adminv1::ForgetRecipeRequest* req, adminv1::ForgetRecipeResponse* resp) override {
        if (auto s = authorize(ctx, "ForgetRecipe"); !s.ok()) return s;
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmRecipeResult out;
        if (auto s = on_net_thread([&] { out = owner.travel_.forget_recipe(req->character(), req->recipe(), by); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_character(out.character);
        resp->set_recipe(out.recipe);
        resp->set_recipe_guid(out.recipe_guid);
        resp->set_taught(out.taught);
        return grpc::Status::OK;
    }

    grpc::Status ResetTutorials(grpc::ServerContext* ctx, const adminv1::ResetTutorialsRequest* req, adminv1::ResetTutorialsResponse* resp) override {
        if (auto s = authorize(ctx, "ResetTutorials"); !s.ok()) return s;
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmTutorialResult out;
        if (auto s = on_net_thread([&] { out = owner.travel_.reset_tutorials(req->character(), by); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_character(out.character);
        resp->set_cleared(static_cast<int32_t>(out.cleared));
        return grpc::Status::OK;
    }

    grpc::Status ListTaughtRecipes(grpc::ServerContext* ctx, const adminv1::ListTaughtRecipesRequest* req, adminv1::ListTaughtRecipesResponse* resp) override {
        if (auto s = authorize(ctx, "ListTaughtRecipes"); !s.ok()) return s;
        GmTaughtList out;
        if (auto s = on_net_thread([&] { out = owner.travel_.list_taught(req->character()); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_character(out.character);
        for (const auto& [name, guid] : out.recipes) {
            auto* r = resp->add_recipes();
            r->set_recipe(name);
            r->set_recipe_guid(guid);
        }
        return grpc::Status::OK;
    }

    grpc::Status PlaceStation(grpc::ServerContext* ctx, const adminv1::PlaceStationRequest* req, adminv1::PlaceStationResponse* resp) override {
        if (auto s = authorize(ctx, "PlaceStation"); !s.ok()) return s;
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmStationResult out;
        if (auto s = on_net_thread([&] { out = owner.travel_.place_station(req->node(), req->plot(), req->station(), by); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_node(out.node);
        resp->set_plot(out.plot);
        resp->set_station(out.station);
        resp->set_actor(out.actor);
        resp->set_building(out.building);
        resp->mutable_position()->set_x(out.position.x);
        resp->mutable_position()->set_y(out.position.y);
        resp->mutable_position()->set_z(out.position.z);
        return grpc::Status::OK;
    }

    // ---- the NPC actions: each runs on the network thread and answers in one plain sentence

    template <class Run>
    grpc::Status npc_action(grpc::ServerContext* ctx, const char* method, Run&& run, GmNpcResult& out) {
        if (auto s = authorize(ctx, method); !s.ok()) return s;
        if (auto s = on_net_thread([&] { out = run("the admin API (" + ctx->peer() + ")"); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        return grpc::Status::OK;
    }

    grpc::Status KillNpc(grpc::ServerContext* ctx, const adminv1::KillNpcRequest* req, adminv1::KillNpcResponse* resp) override {
        GmNpcResult out;
        if (auto s = npc_action(ctx, "KillNpc", [&](const std::string& by) { return owner.travel_.kill_npc(req->id(), by); }, out); !s.ok()) return s;
        resp->set_id(out.id);
        resp->set_name(out.name);
        put(resp->mutable_position(), out.position);
        resp->set_result(out.message);
        resp->set_loot_stacks(out.loot_stacks);
        resp->set_respawn_in_seconds(out.respawn_in_seconds);
        return grpc::Status::OK;
    }

    grpc::Status RespawnNpc(grpc::ServerContext* ctx, const adminv1::RespawnNpcRequest* req, adminv1::RespawnNpcResponse* resp) override {
        GmNpcResult out;
        if (auto s = npc_action(ctx, "RespawnNpc", [&](const std::string& by) { return owner.travel_.respawn_npc(req->id(), by); }, out); !s.ok()) return s;
        resp->set_id(out.id);
        resp->set_name(out.name);
        put(resp->mutable_position(), out.position);
        resp->set_result(out.message);
        return grpc::Status::OK;
    }

    grpc::Status ResetNpc(grpc::ServerContext* ctx, const adminv1::ResetNpcRequest* req, adminv1::ResetNpcResponse* resp) override {
        GmNpcResult out;
        if (auto s = npc_action(ctx, "ResetNpc", [&](const std::string& by) { return owner.travel_.reset_npc(req->id(), by); }, out); !s.ok()) return s;
        resp->set_id(out.id);
        resp->set_name(out.name);
        put(resp->mutable_position(), out.position);
        resp->set_result(out.message);
        return grpc::Status::OK;
    }

    grpc::Status HealNpc(grpc::ServerContext* ctx, const adminv1::HealNpcRequest* req, adminv1::HealNpcResponse* resp) override {
        GmNpcResult out;
        if (auto s = npc_action(ctx, "HealNpc", [&](const std::string& by) { return owner.travel_.heal_npc(req->id(), by); }, out); !s.ok()) return s;
        resp->set_id(out.id);
        resp->set_name(out.name);
        put(resp->mutable_position(), out.position);
        resp->set_result(out.message);
        return grpc::Status::OK;
    }

    grpc::Status DespawnNpc(grpc::ServerContext* ctx, const adminv1::DespawnNpcRequest* req, adminv1::DespawnNpcResponse* resp) override {
        GmNpcResult out;
        if (auto s = npc_action(ctx, "DespawnNpc", [&](const std::string& by) { return owner.travel_.despawn_npc(req->id(), by); }, out); !s.ok()) return s;
        resp->set_id(out.id);
        resp->set_name(out.name);
        put(resp->mutable_position(), out.position);
        resp->set_result(out.message);
        resp->set_respawn_in_seconds(out.respawn_in_seconds);
        return grpc::Status::OK;
    }

    grpc::Status MoveNpc(grpc::ServerContext* ctx, const adminv1::MoveNpcRequest* req, adminv1::MoveNpcResponse* resp) override {
        if (!req->has_point()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "give a point");
        GmNpcResult out;
        const adminv1::Point& p = req->point();
        if (auto s = npc_action(ctx, "MoveNpc", [&](const std::string& by) { return owner.travel_.move_npc(req->id(), p.x(), p.y(), p.z(), p.has_z(), by); }, out); !s.ok()) return s;
        resp->set_id(out.id);
        resp->set_name(out.name);
        put(resp->mutable_position(), out.position);
        resp->set_result(out.message);
        put(resp->mutable_from(), out.from);
        return grpc::Status::OK;
    }

    grpc::Status RemoveStation(grpc::ServerContext* ctx, const adminv1::RemoveStationRequest* req, adminv1::RemoveStationResponse* resp) override {
        if (auto s = authorize(ctx, "RemoveStation"); !s.ok()) return s;
        const std::string by = "the admin API (" + ctx->peer() + ")";
        GmStationResult out;
        if (auto s = on_net_thread([&] { out = owner.travel_.remove_station(req->node(), req->plot(), by); }); !s.ok()) return s;
        if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
        resp->set_node(out.node);
        resp->set_plot(out.plot);
        resp->set_station(out.station);
        return grpc::Status::OK;
    }
};

WorldAdminServer::WorldAdminServer(GmTravel& travel, AdminQueue& queue, Options options)
    : travel_(travel), queue_(queue), options_(std::move(options)), service_(std::make_unique<Service>(*this)) {}

WorldAdminServer::~WorldAdminServer() { stop(); }

bool WorldAdminServer::start(std::string* error) {
    if (server_) return true;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(options_.address, grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service_.get());
    builder.SetMaxReceiveMessageSize(64 * 1024);
    grpc::ResourceQuota quota;
    quota.SetMaxThreads(8);   // a handful of callers: a flood costs threads, not the process
    builder.SetResourceQuota(quota);
    server_ = builder.BuildAndStart();
    if (!server_ || port == 0) {
        server_.reset();
        if (error) *error = "cannot listen on " + options_.address + " (is another worldd running? set AOC_WORLD_ADMIN_ADDR)";
        return false;
    }
    port_ = port;
    return true;
}

void WorldAdminServer::stop() {
    queue_.close();
    if (server_) {
        server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        server_.reset();
    }
}

uint64_t WorldAdminServer::refused() const { return service_->refused_calls.load(); }

}}}  // namespace aoc::worldd::svc

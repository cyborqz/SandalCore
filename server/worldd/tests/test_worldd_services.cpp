// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_services.cpp: svc/grpc_services against in-process gRPC fakes
//  of the gateway, character and mesh services.
// ============================================================================
#include "svc/grpc_services.h"
#include "svc/service_token.h"
#include "aoc/character/v1/character.grpc.pb.h"
#include "aoc/gateway/v1/gateway.grpc.pb.h"
#include "aoc/mesh/v1/mesh.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace aoc::worldd;
namespace gatewayv1 = aoc::gateway::v1;
namespace characterv1 = aoc::character::v1;
namespace meshv1 = aoc::mesh::v1;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const std::string kToken(64, 'a');

static std::string auth_of(grpc::ServerContext* ctx) {
    const auto& md = ctx->client_metadata();
    auto it = md.find(svc::kAuthorizationMetadata);
    return it == md.end() ? "" : std::string(it->second.data(), it->second.size());
}

struct Gateway final : gatewayv1::GatewayService::Service {
    grpc::StatusCode code = grpc::StatusCode::OK;
    std::string auth, ticket, character;
    uint32_t server = 0;
    std::chrono::system_clock::time_point deadline;
    grpc::Status RedeemTicket(grpc::ServerContext* ctx, const gatewayv1::RedeemTicketRequest* req,
                              gatewayv1::RedeemTicketResponse* resp) override {
        auth = auth_of(ctx);
        deadline = ctx->deadline();
        ticket = req->ticket();
        character = req->character_id();
        server = req->server_id();
        if (code != grpc::StatusCode::OK) return grpc::Status(code, "no");
        resp->set_account_id(3);
        resp->set_character_id(req->character_id());
        return grpc::Status::OK;
    }
};

struct SavedState {
    std::string id;
    bool has_position = false;
    double x = 0, y = 0, z = 0;
    float yaw = 0;
    std::string last_region;
    int32_t level = 0;
    int calls = 0;
};
// C.O.A with C Y B

struct Characters final : characterv1::CharacterService::Service {
    grpc::StatusCode code = grpc::StatusCode::OK;
    grpc::StatusCode save_code = grpc::StatusCode::OK;
    std::chrono::milliseconds save_delay{0};   // a service that hangs on SaveCharacterState
    bool with_position = true;
    std::string auth;
    SavedState saved;
    grpc::Status SaveCharacterState(grpc::ServerContext* ctx, const characterv1::SaveCharacterStateRequest* req,
                                    characterv1::SaveCharacterStateResponse*) override {
        auth = auth_of(ctx);
        ++saved.calls;
        if (save_delay.count() > 0) std::this_thread::sleep_for(save_delay);
        saved.id = req->id();
        saved.has_position = req->has_position();
        saved.x = req->position().x();
        saved.y = req->position().y();
        saved.z = req->position().z();
        saved.yaw = req->position().yaw();
        saved.last_region = req->last_region();
        saved.level = req->level();
        return save_code == grpc::StatusCode::OK ? grpc::Status::OK : grpc::Status(save_code, "no");
    }
    grpc::Status GetCharacter(grpc::ServerContext* ctx, const characterv1::GetCharacterRequest* req,
                              characterv1::GetCharacterResponse* resp) override {
        auth = auth_of(ctx);
        if (code != grpc::StatusCode::OK) return grpc::Status(code, "no");
        auto* c = resp->mutable_character();
        c->set_id(req->id());
        c->set_account_id(3);
        c->set_name("cyb");
        c->set_race_id(6);
        c->set_gender_id(1);
        c->set_class_id(7);
        c->set_archetype_id(17754);
        c->set_class_name("Bard");
        c->set_appearance_json("{}");
        c->set_level(5);
        c->set_last_region("Riverlands");
        if (with_position) {
            c->mutable_position()->set_x(1.5);
            c->mutable_position()->set_y(-2.5);
            c->mutable_position()->set_z(3.0);
            c->mutable_position()->set_yaw(90.0f);
        }
        return grpc::Status::OK;
    }
};

struct Mesh final : meshv1::MeshService::Service {
    grpc::StatusCode heartbeat_code = grpc::StatusCode::OK;
    std::string auth, address;
    uint32_t heartbeat_id = 0;
    grpc::Status Register(grpc::ServerContext* ctx, const meshv1::RegisterRequest* req,
                          meshv1::RegisterResponse* resp) override {
        auth = auth_of(ctx);
        address = req->address();
        resp->set_server_id(7);
        resp->set_heartbeat_interval_ms(5000);
        return grpc::Status::OK;
    }
    grpc::Status Heartbeat(grpc::ServerContext* ctx, const meshv1::HeartbeatRequest* req,
                           meshv1::HeartbeatResponse*) override {
        auth = auth_of(ctx);
        heartbeat_id = req->server_id();
        return heartbeat_code == grpc::StatusCode::OK ? grpc::Status::OK : grpc::Status(heartbeat_code, "no");
    }
};

int main() {
    Gateway gw;
    Characters ch;
    Mesh mesh;
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&gw);
    b.RegisterService(&ch);
    b.RegisterService(&mesh);
    auto server = b.BuildAndStart();
    CHECK(server && port > 0, "fake services listen");
    if (!server) return 1;
    const std::string addr = "127.0.0.1:" + std::to_string(port);
    svc::GrpcServices s({addr, addr, addr}, kToken, std::chrono::seconds(2));

    std::printf("gateway\n");
    const auto before = std::chrono::system_clock::now();
    auto r = s.redeem_ticket("0123456789abcdef0123456789abcdef", "bbec5c86eb101fe5f38c87f0fa22e7d0", 7);
    CHECK(r.status == svc::CallStatus::Ok && r.account_id == 3 && r.character_id == "bbec5c86eb101fe5f38c87f0fa22e7d0",
          "RedeemTicket returns the account and character");
    CHECK(gw.auth == "Bearer " + kToken, "the call carries the service token");
    CHECK(gw.ticket == "0123456789abcdef0123456789abcdef" && gw.server == 7, "ticket and server id are sent");
    CHECK(gw.deadline > before && gw.deadline <= before + std::chrono::seconds(3), "the call has a 2 s deadline");
    gw.code = grpc::StatusCode::PERMISSION_DENIED;
    CHECK(s.redeem_ticket("x", "y", 7).status == svc::CallStatus::Refused, "PermissionDenied -> Refused");
    gw.code = grpc::StatusCode::UNAUTHENTICATED;
    CHECK(s.redeem_ticket("x", "y", 7).status == svc::CallStatus::Failed,
          "Unauthenticated (a wrong service token) -> Failed");

    std::printf("character\n");
    auto c = s.get_character("bbec5c86eb101fe5f38c87f0fa22e7d0");
    CHECK(c.status == svc::CallStatus::Ok && c.character.name == "cyb" && c.character.account_id == 3 &&
          c.character.race_id == 6 && c.character.gender_id == 1 && c.character.class_id == 7 && c.character.archetype_id == 17754 &&
          c.character.class_name == "Bard" && c.character.level == 5 && c.character.last_region == "Riverlands",
          "GetCharacter maps the record");
    CHECK(c.character.has_position && c.character.position.x == 1.5 && c.character.position.y == -2.5 &&
          c.character.position.yaw == 90.0f,
          "with its position");
    ch.with_position = false;
    CHECK(!s.get_character("x").character.has_position, "no position until the first save");
    ch.code = grpc::StatusCode::NOT_FOUND;
    CHECK(s.get_character("x").status == svc::CallStatus::NotFound, "NotFound -> NotFound");
    CHECK(ch.auth == "Bearer " + kToken, "it carries the token too");

    std::printf("save_position (plan A6)\n");
    ch.auth.clear();
    CHECK(s.save_position("bbec5c86eb101fe5f38c87f0fa22e7d0", {-777664.5, 616578.6875, 15952.0498046875, 90.9f}) == svc::CallStatus::Ok,
          "SaveCharacterState with a position: Ok");
    CHECK(ch.saved.calls == 1 && ch.saved.id == "bbec5c86eb101fe5f38c87f0fa22e7d0" && ch.saved.has_position && ch.saved.x == -777664.5 &&
              ch.saved.y == 616578.6875 && ch.saved.z == 15952.0498046875 && ch.saved.yaw == 90.9f,
          "the request has the id and x, y, z (doubles) and the yaw (float) as given");
    CHECK(ch.saved.last_region.empty() && ch.saved.level == 0,
          "last_region and level are left unset: the service keeps them (character.proto)");
    CHECK(ch.auth == "Bearer " + kToken, "the save carries the service token");
    ch.save_code = grpc::StatusCode::NOT_FOUND;
    CHECK(s.save_position("x", {}) == svc::CallStatus::NotFound, "NotFound -> NotFound");
    ch.save_code = grpc::StatusCode::INVALID_ARGUMENT;
    CHECK(s.save_position("x", {}) == svc::CallStatus::Refused, "InvalidArgument (a position the store rejects) -> Refused");
    ch.save_code = grpc::StatusCode::UNAVAILABLE;
    CHECK(s.save_position("x", {}) == svc::CallStatus::Unavailable, "Unavailable -> Unavailable (worth retrying)");
    ch.save_code = grpc::StatusCode::OK;
    ch.code = grpc::StatusCode::OK;
    {
        // the save has a deadline of its own, shorter than the other calls' 5 s (the saver's worker is waited for when worldd stops):
        // 200 ms here; the default is 2 s
        svc::GrpcServices impatient({addr, addr, addr}, kToken, std::chrono::seconds(3), std::chrono::milliseconds(200));
        ch.save_delay = std::chrono::milliseconds(1200);
        const auto begin = std::chrono::steady_clock::now();
        const svc::CallStatus status = impatient.save_position("x", {});
        const auto took = std::chrono::steady_clock::now() - begin;
        CHECK(status == svc::CallStatus::Unavailable && took < std::chrono::milliseconds(900),
              "a save to a service that hangs gives up at its own deadline (Unavailable, retried by the saver), not at the 3 s of the other calls");
        ch.save_delay = std::chrono::milliseconds(0);
        CHECK(impatient.save_position("x", {}) == svc::CallStatus::Ok && impatient.get_character("x").status == svc::CallStatus::Ok,
              "(a service that answers is saved to, and the other calls keep their own deadline)");
    }
// (C | O | A | C Y B)

    std::printf("mesh\n");
    auto reg = s.register_server("127.0.0.1:7777", {});
    CHECK(reg.status == svc::CallStatus::Ok && reg.server_id == 7 && reg.heartbeat_interval_ms == 5000 &&
          mesh.address == "127.0.0.1:7777",
          "Register returns the server id and heartbeat interval");
    CHECK(s.heartbeat(7) == svc::CallStatus::Ok && mesh.heartbeat_id == 7, "Heartbeat");
    mesh.heartbeat_code = grpc::StatusCode::NOT_FOUND;
    CHECK(s.heartbeat(7) == svc::CallStatus::NotFound, "Heartbeat NotFound: mesh forgot the server");
    CHECK(mesh.auth == "Bearer " + kToken, "mesh calls carry the token");

    std::printf("status mapping and failures\n");
    CHECK(svc::map_grpc_status(grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "")) == svc::CallStatus::Unavailable &&
          svc::map_grpc_status(grpc::Status(grpc::StatusCode::UNAVAILABLE, "")) == svc::CallStatus::Unavailable &&
          svc::map_grpc_status(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "")) == svc::CallStatus::Refused &&
          svc::map_grpc_status(grpc::Status(grpc::StatusCode::INTERNAL, "")) == svc::CallStatus::Failed,
          "deadline and unavailable retry; invalid argument is a refusal; internal fails");
    svc::GrpcServices down({"127.0.0.1:1", "127.0.0.1:1", "127.0.0.1:1"}, kToken, std::chrono::milliseconds(300));
    CHECK(down.redeem_ticket("x", "y", 1).status == svc::CallStatus::Unavailable, "an unreachable service -> Unavailable");
    server->Shutdown();

    std::printf("service token file\n");
    std::string err;
    const std::string path = "test_worldd_services.token";
    { std::ofstream(path) << "  " << kToken << "\r\n"; }
    const auto tok = svc::load_service_token(path, &err);
    CHECK(tok && *tok == kToken, "loads 64 hex digits, ignoring whitespace");
    { std::ofstream(path) << "ABC"; }
    CHECK(!svc::load_service_token(path, &err) && err.find("ABC") == std::string::npos,
          "refuses anything else, without quoting the file");
    std::remove(path.c_str());
    CHECK(!svc::load_service_token(path, &err) && err.find("services.ps1") != std::string::npos,
          "a missing file names the script that creates it");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

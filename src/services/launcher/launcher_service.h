// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>
#include <string>
#include <chrono>

#include "ics_launcher.grpc.pb.h"

/// Emulated LauncherService: tells the client to connect to a local game server.
class LauncherServiceImpl final : public ics_launcher::LauncherService::Service {
public:
    /// Configuration for the emulated server
    struct Config {
        std::string game_server_ip   = "127.0.0.1";
        int         game_server_port = 7777;
        std::string branch_id        = "main";
        std::string executable       = "AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe";
        std::string arguments        = "";    // will be built in GetGameClientConnectionInfos
        std::string file_storage     = "";    // game install path
    };

    explicit LauncherServiceImpl(Config config = {})
        : config_(std::move(config)) {}

    // ── NDA check (always signed) ───────────────────────────────────────────
    grpc::Status IsNdaSigned(
        grpc::ServerContext* context,
        const ics_launcher::IsNdaSignedRequest* request,
        ics_launcher::IsNdaSignedReply* reply) override
    {
        spdlog::info("[LauncherService] IsNdaSigned: product_id='{}'",
                     request->product_id());

        auto* sys = reply->mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";

        reply->set_result_code(ics_launcher::LAUNCHER_RESULT_CODE_SUCCESS);
        reply->set_is_signed(true);  // Always signed for emulation

        return grpc::Status::OK;
    }

    // ── Sign NDA (always succeeds) ──────────────────────────────────────────
    grpc::Status SignNda(
        grpc::ServerContext* context,
        const ics_launcher::SignNdaRequest* request,
        ics_launcher::SignNdaReply* reply) override
    {
        spdlog::info("[LauncherService] SignNda: product_id='{}'",
                     request->product_id());

        auto* sys = reply->mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";

        reply->set_result_code(ics_launcher::LAUNCHER_RESULT_CODE_SUCCESS);

        return grpc::Status::OK;
    }

    // ── Get game client connection info (THE KEY RPC) ───────────────────────
    grpc::Status GetGameClientConnectionInfos(
        grpc::ServerContext* context,
        const ics_launcher::GetGameClientConnectionInfosRequest* request,
        ics_launcher::GetGameClientConnectionInfosReply* reply) override
    {
        spdlog::info("[LauncherService] GetGameClientConnectionInfos: server_id='{}'",
                     request->server_id());
// (C:Y:B) (CoA)

        auto* sys = reply->mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";

        reply->set_result_code(ics_launcher::LAUNCHER_RESULT_CODE_SUCCESS);

        auto* infos = reply->mutable_infos();
        infos->set_branch_id(config_.branch_id);
        infos->set_executable(config_.executable);

        // Build connection arguments for UE5 game client
        // Typical UE format: <IP>:<Port> -AUTH_TOKEN=xxx
        std::string args = config_.game_server_ip + ":"
                         + std::to_string(config_.game_server_port);
        if (!config_.arguments.empty()) {
            args += " " + config_.arguments;
        }
        infos->set_arguments(args);
        infos->set_file_storage_location(config_.file_storage);

        spdlog::info("[LauncherService] Returning connection: {}:{} exe='{}'",
                     config_.game_server_ip, config_.game_server_port,
                     config_.executable);

        return grpc::Status::OK;
    }

    // ── Get URL from token ──────────────────────────────────────────────────
    grpc::Status GetUrlFromToken(
        grpc::ServerContext* context,
        const ics_launcher::GetUrlFromTokenRequest* request,
        ics_launcher::GetUrlFromTokenReply* reply) override
    {
        spdlog::info("[LauncherService] GetUrlFromToken: token='{}'",
                     request->token().substr(0, 20) + "...");

        auto* sys = reply->mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";

        reply->set_result_code(ics_launcher::LAUNCHER_RESULT_CODE_SUCCESS);

        auto* url = reply->mutable_resource_url();
        url->set_url("https://localhost/emu/resource");
        url->set_expire_in(3600);

        return grpc::Status::OK;
    }

    // ── Get patch to latest version (V1) - no patch needed ──────────────────
    grpc::Status GetGamePatchToLatestVersion(
        grpc::ServerContext* context,
        const ics_launcher::GetGamePatchToLatestVersionRequest* request,
        ics_launcher::GetGamePatchToLatestVersionReply* reply) override
    {
        spdlog::info("[LauncherService] GetGamePatchToLatestVersion: branch='{}' hash_from='{}'",
                     request->branch_id(), request->hash_from());

        auto* sys = reply->mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";

        // Return success with no patch needed (empty game_patch = up to date)
        reply->set_result_code(ics_launcher::LAUNCHER_RESULT_CODE_ERROR_NO_PATCH_TO_TARGET_VERSION);

        spdlog::info("[LauncherService] No patch needed (emulation mode)");
        return grpc::Status::OK;
    }

    // ── Get patch to latest version (V2) - no patch needed ──────────────────
    grpc::Status GetGamePatchToLatestVersion_v2(
        grpc::ServerContext* context,
        const ics_launcher::GetGamePatchToLatestVersionRequest_v2* request,
        ics_launcher::GetGamePatchToLatestVersionReply_v2* reply) override
    {
        spdlog::info("[LauncherService] GetGamePatchToLatestVersion_v2: branch='{}' hash_from='{}'",
                     request->branch_id(), request->hash_from());

        auto* sys = reply->mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";

        reply->set_result_code(ics_launcher::LAUNCHER_RESULT_CODE_ERROR_NO_PATCH_TO_TARGET_VERSION);

        spdlog::info("[LauncherService] No patch needed v2 (emulation mode)");
        return grpc::Status::OK;
    }

    // ── Bidirectional streaming for launcher messages ───────────────────────
    grpc::Status ProcessLauncherMessage(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<ics_common::MessageWrapper,
                                 ics_common::MessageWrapper>* stream) override
    {
        spdlog::info("[LauncherService] ProcessLauncherMessage stream opened");

        ics_common::MessageWrapper incoming;
        while (stream->Read(&incoming)) {
            spdlog::info("[LauncherService] Stream msg: type='{}' status={}",
                         incoming.message_type_name(),
                         static_cast<int>(incoming.status_code()));

            // Handle SessionRequest
            if (incoming.message_type_name() == "ics_common.SessionRequest") {
                ics_common::SessionRequest session_req;
                if (session_req.ParseFromString(incoming.message_data())) {
                    spdlog::info("[LauncherService] SessionRequest type={}",
                                 static_cast<int>(session_req.request_type()));

                    ics_common::SessionReply session_reply;
                    session_reply.set_result_code(ics_common::ICS_STATUS_CODE_SUCCESS);
                    session_reply.set_request_type(session_req.request_type());

                    ics_common::MessageWrapper response;
                    auto* sys = response.mutable_system_data();
                    (*sys->mutable_tags())["emu"] = "true";
                    response.set_status_code(ics_common::ICS_STATUS_CODE_SUCCESS);
                    response.set_message_type_name("ics_common.SessionReply");
                    response.set_message_data(session_reply.SerializeAsString());
                    stream->Write(response);
                }
            }
            // Handle KeepAlive
            else if (incoming.message_type_name() == "ics_common.KeepAliveMessage") {
                ics_common::MessageWrapper response;
                auto* sys = response.mutable_system_data();
                (*sys->mutable_tags())["emu"] = "true";
                response.set_status_code(ics_common::ICS_STATUS_CODE_SUCCESS);
                response.set_message_type_name("ics_common.KeepAliveMessage");

                ics_common::KeepAliveMessage ka;
                ka.set_timestamp_msec(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count()
                );
                response.set_message_data(ka.SerializeAsString());
                stream->Write(response);
            }
            else {
                // Echo back with success
                ics_common::MessageWrapper response;
                auto* sys = response.mutable_system_data();
                (*sys->mutable_tags())["emu"] = "true";
                response.set_status_code(ics_common::ICS_STATUS_CODE_SUCCESS);
                response.set_message_type_name(incoming.message_type_name());
                stream->Write(response);
            }
        }

        spdlog::info("[LauncherService] ProcessLauncherMessage stream closed");
        return grpc::Status::OK;
    }

private:
    Config config_;
};

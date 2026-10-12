// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>
#include <string>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>
#include <iomanip>
#include <sstream>

#include "ics_auth.grpc.pb.h"
#include "util/proxy_logger.h"

/// AuthService: emulation mode OR proxy mode (forwards to real Intrepid server).
class AuthServiceImpl final : public ics_auth::AuthService::Service {
public:
    AuthServiceImpl() = default;

    void set_proxy_channel(std::shared_ptr<grpc::Channel> channel) {
        proxy_channel_ = std::move(channel);
        proxy_stub_ = ics_auth::AuthService::NewStub(proxy_channel_);
        spdlog::info("[AuthService] PROXY MODE enabled");
    }

    bool is_proxy_mode() const { return proxy_stub_ != nullptr; }

    // Forward client metadata to upstream context
    // Only skip gRPC-internal headers and HTTP/2 pseudo-headers
    void forward_metadata(grpc::ServerContext* server_ctx, grpc::ClientContext* upstream_ctx) {
        spdlog::info("[AuthService] [PROXY] Client metadata ({} entries):",
                     server_ctx->client_metadata().size());
        for (const auto& md : server_ctx->client_metadata()) {
            std::string key(md.first.data(), md.first.size());
            std::string val(md.second.data(), md.second.size());
            std::string display_val = val.size() > 100 ? val.substr(0,100) + "..." : val;

            // Skip gRPC-internal, HTTP/2 pseudo-headers, and transport headers
            // user-agent is set on the channel via GRPC_ARG_PRIMARY_USER_AGENT_STRING
            // so forwarding it via metadata can cause duplication/conflicts
            if (key.find("grpc-") == 0 || (!key.empty() && key[0] == ':') ||
                key == "user-agent" || key == "te" || key == "content-type") {
                spdlog::debug("[AuthService] [PROXY]   SKIP: {}={}", key, display_val);
                continue;
            }
            upstream_ctx->AddMetadata(key, val);
            spdlog::info("[AuthService] [PROXY]   FWD:  {}={}", key, display_val);
        }
    }

    // Log channel state for diagnostics
    void log_channel_state(const char* rpc_name) {
        if (!proxy_channel_) return;
        auto state = proxy_channel_->GetState(true);
        const char* state_str = "UNKNOWN";
        switch (state) {
            case GRPC_CHANNEL_IDLE: state_str = "IDLE"; break;
            case GRPC_CHANNEL_CONNECTING: state_str = "CONNECTING"; break;
            case GRPC_CHANNEL_READY: state_str = "READY"; break;
            case GRPC_CHANNEL_TRANSIENT_FAILURE: state_str = "TRANSIENT_FAILURE"; break;
            case GRPC_CHANNEL_SHUTDOWN: state_str = "SHUTDOWN"; break;
        }
        spdlog::info("[AuthService] [PROXY] Channel state before {}: {}", rpc_name, state_str);
    }
// [CoA x C_Y_B]

    // ── Standard username/password auth ─────────────────────────────────────
    grpc::Status ProcessUnaryAuthBeginRequest(
        grpc::ServerContext* context,
        const ics_auth::UnaryAuthBeginRequest* request,
        ics_auth::UnaryAuthBeginReply* reply) override
    {
        spdlog::info("[AuthService] ProcessUnaryAuthBeginRequest: user='{}'",
                     request->username());

        if (proxy_stub_) {
            log_channel_state("AuthBeginRequest");
            grpc::ClientContext upstream_ctx;
            forward_metadata(context, &upstream_ctx);
            upstream_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
            auto status = proxy_stub_->ProcessUnaryAuthBeginRequest(
                &upstream_ctx, *request, reply);
            log_auth_reply("AuthBeginReply", reply->result_code(),
                           reply->auth_token(), reply->refresh_token(), status);
            if (!status.ok()) {
                spdlog::error("[AuthService] [PROXY] FAILED: code={} msg='{}' details='{}'",
                              (int)status.error_code(), status.error_message(), status.error_details());
            }
            return status;
        }

        auto* sys = reply->mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";
        reply->set_result_code(ics_auth::AUTH_RESULT_CODE_SUCCESS);
        reply->set_auth_token(generate_jwt("auth"));
        reply->set_refresh_token(generate_jwt("refresh"));
        spdlog::info("[AuthService] Auth success for user '{}' (JWT)", request->username());
        return grpc::Status::OK;
    }

    // ── Steam ticket authentication (primary path) ──────────────────────────
    grpc::Status ProcessUnaryAuthBeginSteamRequest(
        grpc::ServerContext* context,
        const ics_auth::UnaryAuthBeginSteamRequest* request,
        ics_auth::UnaryAuthBeginSteamReply* reply) override
    {
        spdlog::info("[AuthService] ProcessUnaryAuthBeginSteamRequest: ticket_len={}",
                     request->steam_ticket().size());

        if (proxy_stub_) {
            log_channel_state("SteamAuthRequest");
            grpc::ClientContext upstream_ctx;
            forward_metadata(context, &upstream_ctx);
            upstream_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
            upstream_ctx.set_wait_for_ready(true);  // Wait for channel to be READY

            // Log the serialized request for forensics
            auto req_wire = request->SerializeAsString();
            spdlog::info("[AuthService] [PROXY] Sending SteamAuthRequest: {}B", req_wire.size());

            auto status = proxy_stub_->ProcessUnaryAuthBeginSteamRequest(
                &upstream_ctx, *request, reply);

            spdlog::info("[AuthService] [PROXY] Steam auth response:");
            spdlog::info("[AuthService] [PROXY]   gRPC status: {} ({})",
                         (int)status.error_code(), status.error_message());

            // Log trailing metadata from upstream (may contain error info)
            auto trailing = upstream_ctx.GetServerTrailingMetadata();
            spdlog::info("[AuthService] [PROXY]   Trailing metadata ({} entries):", trailing.size());
            for (const auto& md : trailing) {
                std::string key(md.first.data(), md.first.size());
                std::string val(md.second.data(), md.second.size());
                spdlog::info("[AuthService] [PROXY]     {}: {}", key,
                    val.size() > 200 ? val.substr(0,200) + "..." : val);
            }
            // Also log initial metadata
            auto initial_md = upstream_ctx.GetServerInitialMetadata();
            spdlog::info("[AuthService] [PROXY]   Initial metadata ({} entries):", initial_md.size());
            for (const auto& md : initial_md) {
                std::string key(md.first.data(), md.first.size());
                std::string val(md.second.data(), md.second.size());
                spdlog::info("[AuthService] [PROXY]     {}: {}", key,
                    val.size() > 200 ? val.substr(0,200) + "..." : val);
            }

            spdlog::info("[AuthService] [PROXY]   result_code: {}",
                         (int)reply->result_code());
            if (!reply->auth_token().empty())
                spdlog::info("[AuthService] [PROXY]   auth_token: '{}'",
                             reply->auth_token().substr(0, 40) + "...");
            if (!reply->refresh_token().empty())
                spdlog::info("[AuthService] [PROXY]   refresh_token: '{}'",
                             reply->refresh_token().substr(0, 40) + "...");
            if (!reply->oauth_auth_token().empty())
                spdlog::info("[AuthService] [PROXY]   oauth_auth_token: '{}'",
                             reply->oauth_auth_token().substr(0, 40) + "...");
            if (!reply->oauth_refresh_token().empty())
                spdlog::info("[AuthService] [PROXY]   oauth_refresh_token: '{}'",
                             reply->oauth_refresh_token().substr(0, 40) + "...");

            if (reply->has_system_data()) {
                std::ostringstream oss;
                for (const auto& kv : reply->system_data().tags())
                    oss << kv.first << ":" << kv.second << ", ";
                spdlog::info("[AuthService] [PROXY]   system_data: tags={{{}}}", oss.str());
            }
// (C O A : Cyb)

            if (!status.ok()) {
                spdlog::error("[AuthService] [PROXY]   ERROR details: '{}'", status.error_details());
                spdlog::error("[AuthService] [PROXY]   Channel state after fail: {}",
                              (int)proxy_channel_->GetState(false));
                spdlog::error("[AuthService] [PROXY]   (0=IDLE, 1=CONNECTING, 2=READY, 3=TRANSIENT_FAILURE, 4=SHUTDOWN)");
            }

            // Dump full wire for forensics
            auto wire = reply->SerializeAsString();
            spdlog::info("[AuthService] [PROXY]   WIRE: {}B hex: {}",
                         wire.size(), hex_dump(wire, 512));

            return status;
        }

        // Emulation mode — generate proper JWTs that the client can parse
        auto auth_jwt    = generate_jwt("auth");
        auto refresh_jwt = generate_jwt("refresh");

        auto* sys = reply->mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";
        reply->set_result_code(ics_auth::AUTH_RESULT_CODE_SUCCESS);
        reply->set_auth_token(auth_jwt);
        reply->set_refresh_token(refresh_jwt);
        reply->set_oauth_auth_token(auth_jwt);
        reply->set_oauth_refresh_token(refresh_jwt);
        spdlog::info("[AuthService] Steam auth success, JWT tokens issued");
        spdlog::debug("[AuthService]   auth_token: {}...{}",
                      auth_jwt.substr(0, 40), auth_jwt.substr(auth_jwt.size()-10));
        return grpc::Status::OK;
    }

    // ── Token refresh ───────────────────────────────────────────────────────
    grpc::Status ProcessUnaryAuthRefreshRequest(
        grpc::ServerContext* context,
        const ics_auth::UnaryAuthRefreshRequest* request,
        ics_auth::UnaryAuthRefreshReply* reply) override
    {
        spdlog::info("[AuthService] ProcessUnaryAuthRefreshRequest");

        if (proxy_stub_) {
            log_channel_state("AuthRefreshRequest");
            grpc::ClientContext upstream_ctx;
            forward_metadata(context, &upstream_ctx);
            upstream_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
            auto status = proxy_stub_->ProcessUnaryAuthRefreshRequest(
                &upstream_ctx, *request, reply);
            log_auth_reply("AuthRefreshReply", reply->result_code(),
                           reply->auth_token(), reply->refresh_token(), status);
            if (!status.ok()) {
                spdlog::error("[AuthService] [PROXY] FAILED: code={} msg='{}' details='{}'",
                              (int)status.error_code(), status.error_message(), status.error_details());
            }
            return status;
        }

        auto* sys = reply->mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";
        reply->set_result_code(ics_auth::AUTH_RESULT_CODE_SUCCESS);
        reply->set_auth_token(generate_jwt("auth"));
        reply->set_refresh_token(generate_jwt("refresh"));
        spdlog::info("[AuthService] Token refresh success (JWT)");
        return grpc::Status::OK;
    }

    // ── Bidirectional streaming (session management) ────────────────────────
    grpc::Status ProcessAuthMessage(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<ics_common::MessageWrapper,
                                 ics_common::MessageWrapper>* stream) override
    {
        spdlog::info("[AuthService] ProcessAuthMessage stream opened");

        if (proxy_stub_) {
            return proxy_bidi_forward(context, "AuthMessage", stream);
        }

        // Emulation mode
        ics_common::MessageWrapper incoming;
        while (stream->Read(&incoming)) {
            spdlog::info("[AuthService] Stream msg: type='{}' status={}",
                         incoming.message_type_name(),
                         static_cast<int>(incoming.status_code()));

            if (incoming.message_type_name() == "ics_common.SessionRequest") {
                ics_common::SessionRequest session_req;
                if (session_req.ParseFromString(incoming.message_data())) {
                    handle_session_request(session_req, stream);
                }
            }
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
                ics_common::MessageWrapper response;
                auto* sys = response.mutable_system_data();
                (*sys->mutable_tags())["emu"] = "true";
                response.set_status_code(ics_common::ICS_STATUS_CODE_SUCCESS);
                response.set_message_type_name(incoming.message_type_name());
                stream->Write(response);
            }
        }

        spdlog::info("[AuthService] ProcessAuthMessage stream closed");
        return grpc::Status::OK;
    }

private:
    std::shared_ptr<grpc::Channel> proxy_channel_;
    std::unique_ptr<ics_auth::AuthService::Stub> proxy_stub_;

    static std::string hex_dump(const std::string& data, size_t max_bytes = 512) {
        std::ostringstream oss;
        size_t len = std::min(data.size(), max_bytes);
        for (size_t i = 0; i < len; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0')
                << (static_cast<unsigned>(data[i]) & 0xFF);
            if (i + 1 < len) oss << ' ';
        }
        if (data.size() > max_bytes) oss << " ... (" << data.size() << " bytes total)";
        return oss.str();
    }

    void log_auth_reply(const char* label, int result_code,
                        const std::string& auth, const std::string& refresh,
                        const grpc::Status& status) {
        spdlog::info("[AuthService] [PROXY] {} gRPC={} result={} auth='{}...' refresh='{}...'",
                     label, (int)status.error_code(), result_code,
                     auth.substr(0, std::min<size_t>(auth.size(), 30)),
                     refresh.substr(0, std::min<size_t>(refresh.size(), 30)));
    }
// C-O-A - Cyb

    grpc::Status proxy_bidi_forward(
        grpc::ServerContext* server_ctx,
        const char* label,
        grpc::ServerReaderWriter<ics_common::MessageWrapper,
                                 ics_common::MessageWrapper>* client_stream)
    {
        spdlog::info("[AuthService] [PROXY] ======= {} PROXY START =======", label);
        ProxyLogger::instance().log_event("auth", std::string("proxy_start_") + label);

        log_channel_state(label);
        grpc::ClientContext upstream_ctx;
        forward_metadata(server_ctx, &upstream_ctx);
        auto upstream_stream = proxy_stub_->ProcessAuthMessage(&upstream_ctx);
        if (!upstream_stream) {
            spdlog::error("[AuthService] [PROXY] Failed to open upstream stream!");
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "proxy: cannot connect");
        }

        // Thread: server → client
        std::thread server_reader([&]() {
            ics_common::MessageWrapper msg;
            int count = 0;
            while (upstream_stream->Read(&msg)) {
                ++count;
                spdlog::info("[AuthService] [PROXY] SERVER >> CLIENT #{} type='{}' data={}B",
                             count, msg.message_type_name(), msg.message_data().size());
                auto wire = msg.SerializeAsString();
                spdlog::info("[AuthService] [PROXY]   WIRE: {}B {}", wire.size(), hex_dump(wire, 256));
                ProxyLogger::instance().log("auth", "S>C", count, msg);
                client_stream->Write(msg);
            }
            spdlog::info("[AuthService] [PROXY] Server reader done ({} msgs)", count);
        });

        // Main: client → server
        {
            ics_common::MessageWrapper msg;
            int count = 0;
            while (client_stream->Read(&msg)) {
                ++count;
                spdlog::info("[AuthService] [PROXY] CLIENT >> SERVER #{} type='{}' data={}B",
                             count, msg.message_type_name(), msg.message_data().size());
                ProxyLogger::instance().log("auth", "C>S", count, msg);
                upstream_stream->Write(msg);
            }
            spdlog::info("[AuthService] [PROXY] Client reader done ({} msgs)", count);
            upstream_stream->WritesDone();
        }

        server_reader.join();
        auto status = upstream_stream->Finish();
        spdlog::info("[AuthService] [PROXY] ======= {} PROXY END (status={}) =======",
                     label, (int)status.error_code());
        ProxyLogger::instance().log_event("auth", std::string("proxy_end_") + label,
            {{"status", (int)status.error_code()}});
        return grpc::Status::OK;
    }

    void handle_session_request(
        const ics_common::SessionRequest& req,
        grpc::ServerReaderWriter<ics_common::MessageWrapper,
                                 ics_common::MessageWrapper>* stream)
    {
        spdlog::info("[AuthService] SessionRequest type={}",
                     static_cast<int>(req.request_type()));
        ics_common::SessionReply session_reply;
        session_reply.set_result_code(ics_common::ICS_STATUS_CODE_SUCCESS);
        session_reply.set_request_type(req.request_type());
        ics_common::MessageWrapper response;
        auto* sys = response.mutable_system_data();
        (*sys->mutable_tags())["emu"] = "true";
        response.set_status_code(ics_common::ICS_STATUS_CODE_SUCCESS);
        response.set_message_type_name("ics_common.SessionReply");
        response.set_message_data(session_reply.SerializeAsString());
        stream->Write(response);
    }

    static std::string base64url_encode(const std::string& input) {
        static const char table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, bits = -6;
        for (unsigned char c : input) {
            val = (val << 8) + c;
            bits += 8;
            while (bits >= 0) {
                out.push_back(table[(val >> bits) & 0x3F]);
                bits -= 6;
            }
        }
        if (bits > -6) out.push_back(table[((val << 8) >> (bits + 8)) & 0x3F]);
        // URL-safe: replace +→- and /→_
        for (auto& c : out) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
        // No padding
        return out;
    }
// -- CoA & C Y B --

    static std::string generate_jwt(const std::string& token_type) {
        // Build a JWT with the structure the AoC client expects:
        // Header: {"alg":"HS256","typ":"JWT"}
        // Payload: {"iss":"ics_go","sub":"auth","exp":...,"iat":...,"jti":"...","User":{...}}
        auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto exp_s = now_s + 1200; // 20 min

        // Generate random hex strings for IDs
        static std::mt19937 rng(std::random_device{}());
        static const char hex[] = "0123456789abcdef";
        std::uniform_int_distribution<> dist(0, 15);

        std::string jti;
        for (int i = 0; i < 32; ++i) jti += hex[dist(rng)];
        std::string account_id;
        for (int i = 0; i < 32; ++i) account_id += hex[dist(rng)];

        std::string header = R"({"alg":"HS256","typ":"JWT"})";

        // Build payload JSON manually
        std::ostringstream p;
        p << R"({"iss":"ics_go","sub":"auth","exp":)" << exp_s
          << R"(,"iat":)" << now_s
          << R"(,"jti":")" << jti << R"(")"
          << R"(,"User":{"token_type":")" << token_type << R"(")"
          << R"(,"username":"emu_player")"
          << R"(,"account_id":")" << account_id << R"(")"
          << R"(,"auth0_access_token":"")"
          << R"(,"auth0_refresh_token":"")"
          << R"(,"auth0_expire_time":)" << exp_s
          << R"(,"is_steam_login":true}})";

        std::string payload = p.str();

        // Fake signature (32 bytes)
        std::string sig(32, '\0');
        for (auto& c : sig) c = static_cast<char>(dist(rng) | (dist(rng) << 4));

        return base64url_encode(header) + "." +
               base64url_encode(payload) + "." +
               base64url_encode(sig);
    }

    static std::string generate_token(const std::string& prefix) {
        static std::mt19937 rng(std::random_device{}());
        static const char charset[] = "0123456789abcdef";
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        std::string token = prefix + "_emu_" + std::to_string(ms) + "_";
        std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
        for (int i = 0; i < 32; ++i) token += charset[dist(rng)];
        return token;
    }
};

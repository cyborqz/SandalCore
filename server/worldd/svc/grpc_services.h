// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/grpc_services.h
//
//  GatewayApi, CharacterApi and MeshApi over gRPC.  Every call carries the
//  service token and a deadline; gRPC status codes map to CallStatus.
//  Thread-safe: each call uses its own ClientContext.
// ============================================================================
#pragma once

#include "svc/services.h"
#include <chrono>
#include <memory>
#include <string>

namespace grpc { class ClientContext; class Status; }

namespace aoc { namespace worldd { namespace svc {

/// Maps a gRPC status to how worldd treats it.  Unauthenticated means the
/// service token is wrong: a setup error, reported as Failed.
CallStatus map_grpc_status(const grpc::Status& s);

class GrpcServices : public GatewayApi, public CharacterApi, public MeshApi {
public:
    struct Addresses {
        std::string gateway;    // gateway's internal gRPC, e.g. 127.0.0.1:50054
        std::string character;  // e.g. 127.0.0.1:50052
        std::string mesh;       // e.g. 127.0.0.1:50053
    };

    /// `timeout` is every call's deadline but save_position's, which has its own (shorter: the saver's worker thread is
    /// waited for when worldd stops, so a hung character service may hold it for one deadline).
    GrpcServices(const Addresses& addresses, std::string token,
                 std::chrono::milliseconds timeout = std::chrono::seconds(5),
                 std::chrono::milliseconds save_timeout = std::chrono::seconds(2));
    ~GrpcServices() override;
// C-O-A captures, Cyb code

    RedeemResult redeem_ticket(const std::string& ticket, const std::string& character_id,
                               uint32_t server_id) override;
    CharacterResult get_character(const std::string& id) override;
    CallStatus save_position(const std::string& id, const Position& position) override;
    CallStatus save_inventory(const std::string& id, const std::vector<InventoryItem>& items) override;
    CallStatus save_progress(const std::string& id, const Progress& progress) override;
    RegisterResult register_server(const std::string& address,
                                   const std::vector<std::string>& regions) override;
    CallStatus heartbeat(uint32_t server_id) override;

private:
    struct Stubs;
    void prepare(grpc::ClientContext& ctx, std::chrono::milliseconds timeout) const;
    void prepare(grpc::ClientContext& ctx) const { prepare(ctx, timeout_); }

    std::unique_ptr<Stubs> stubs_;
    std::string token_;
    std::chrono::milliseconds timeout_;
    std::chrono::milliseconds save_timeout_;
};

}}} // namespace aoc::worldd::svc

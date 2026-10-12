// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/grpc_services.cpp
// ============================================================================
#include "svc/grpc_services.h"
#include "svc/service_token.h"
#include "aoc/character/v1/character.grpc.pb.h"
#include "aoc/gateway/v1/gateway.grpc.pb.h"
#include "aoc/mesh/v1/mesh.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace aoc { namespace worldd { namespace svc {

namespace characterv1 = ::aoc::character::v1;
namespace gatewayv1 = ::aoc::gateway::v1;
namespace meshv1 = ::aoc::mesh::v1;

struct GrpcServices::Stubs {
    std::unique_ptr<gatewayv1::GatewayService::Stub> gateway;
    std::unique_ptr<characterv1::CharacterService::Stub> character;
    std::unique_ptr<meshv1::MeshService::Stub> mesh;
};

namespace {

std::shared_ptr<grpc::Channel> channel(const std::string& address) {
    grpc::ChannelArguments args;
    // Come back quickly after a service restarts (gRPC's default backoff
    // grows to 120 s).
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 200);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 200);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 5000);
    return grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);
}

}  // namespace

CallStatus map_grpc_status(const grpc::Status& s) {
    switch (s.error_code()) {
    case grpc::StatusCode::OK: return CallStatus::Ok;
    case grpc::StatusCode::PERMISSION_DENIED:
    case grpc::StatusCode::INVALID_ARGUMENT: return CallStatus::Refused;
    case grpc::StatusCode::NOT_FOUND: return CallStatus::NotFound;
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
    case grpc::StatusCode::CANCELLED:
    case grpc::StatusCode::RESOURCE_EXHAUSTED: return CallStatus::Unavailable;
    default: return CallStatus::Failed;
    }
}

GrpcServices::GrpcServices(const Addresses& addresses, std::string token,
                           std::chrono::milliseconds timeout, std::chrono::milliseconds save_timeout)
    : stubs_(std::make_unique<Stubs>()), token_(std::move(token)), timeout_(timeout), save_timeout_(save_timeout) {
    stubs_->gateway = gatewayv1::GatewayService::NewStub(channel(addresses.gateway));
    stubs_->character = characterv1::CharacterService::NewStub(channel(addresses.character));
    stubs_->mesh = meshv1::MeshService::NewStub(channel(addresses.mesh));
}

GrpcServices::~GrpcServices() = default;

void GrpcServices::prepare(grpc::ClientContext& ctx, std::chrono::milliseconds timeout) const {
    ctx.set_deadline(std::chrono::system_clock::now() + timeout);
    ctx.AddMetadata(kAuthorizationMetadata, "Bearer " + token_);
}

RedeemResult GrpcServices::redeem_ticket(const std::string& ticket, const std::string& character_id,
                                         uint32_t server_id) {
    grpc::ClientContext ctx;
    prepare(ctx);
    gatewayv1::RedeemTicketRequest req;
    req.set_ticket(ticket);
    req.set_character_id(character_id);
    req.set_server_id(server_id);
    gatewayv1::RedeemTicketResponse resp;
    RedeemResult r;
    r.status = map_grpc_status(stubs_->gateway->RedeemTicket(&ctx, req, &resp));
    if (r.status == CallStatus::Ok) {
        r.account_id = resp.account_id();
        r.character_id = resp.character_id();
    }
    return r;
}
// (C.O.A) (Cyb)

CharacterResult GrpcServices::get_character(const std::string& id) {
    grpc::ClientContext ctx;
    prepare(ctx);
    characterv1::GetCharacterRequest req;
    req.set_id(id);
    req.set_include_items(true);   // plan B2: worldd plays the character, so it needs its items
    characterv1::GetCharacterResponse resp;
    CharacterResult r;
    r.status = map_grpc_status(stubs_->character->GetCharacter(&ctx, req, &resp));
    if (r.status != CallStatus::Ok) return r;
    const auto& c = resp.character();
    r.character.id = c.id();
    r.character.account_id = c.account_id();
    r.character.name = c.name();
    r.character.race_id = c.race_id();
    r.character.gender_id = c.gender_id();
    r.character.class_id = c.class_id();
    r.character.archetype_id = c.archetype_id();
    r.character.class_name = c.class_name();
    r.character.appearance_json = c.appearance_json();
    r.character.level = c.level();
    r.character.has_position = c.has_position();
    if (c.has_position()) {
        r.character.position = {c.position().x(), c.position().y(), c.position().z(), c.position().yaw()};
    }
    r.character.last_region = c.last_region();
    for (const auto& item : c.items()) {
        InventoryItem it;
        switch (item.container()) {
        case characterv1::ITEM_CONTAINER_BACKPACK: it.container = ItemContainer::Backpack; break;
        case characterv1::ITEM_CONTAINER_EQUIPMENT: it.container = ItemContainer::Equipment; break;
        case characterv1::ITEM_CONTAINER_QUEST: it.container = ItemContainer::Quest; break;
        default:
            // an item in a storage this worldd has never heard of: the whole record is not one it can play
            r.status = CallStatus::Failed;
            r.character.items.clear();
            return r;
        }
        it.slot = item.slot();
        it.item_record_id = item.item_record_id();
        it.instance_guid = item.instance_guid();
        it.quantity = item.quantity();
        it.quality = item.quality();
        it.durability = item.durability();
        it.created_at_ms = item.created_at_ms();
        if (item.has_persisted_health()) it.persisted_health = item.persisted_health();
        if (item.has_persisted_mana()) it.persisted_mana = item.persisted_mana();
        it.lock_expiration = item.lock_expiration();
        r.character.items.push_back(std::move(it));
    }
    for (const auto& e : c.experience()) {
        Experience x;
        x.leveling_type_guid = e.leveling_type_guid();
        x.xp = e.xp();
        x.debt = e.debt();
        r.character.experience.push_back(x);
    }
    for (const auto& t : c.skill_ranks()) {
        TreeRanks tr;
        tr.tree_guid = t.tree_guid();
        tr.rank_guids.assign(t.rank_guids().begin(), t.rank_guids().end());
        r.character.skill_ranks.push_back(std::move(tr));
    }
    for (const auto& a : c.action_bar()) {
        ActionbarSlot s;
        s.bank = a.bank();
        s.slot = a.slot();
        s.use_type = a.use_type();
        s.instigator = a.instigator();
        s.ability_id = a.ability_id();
        s.skill_rank_id = a.skill_rank_id();
        s.pet_command_id = a.pet_command_id();
        s.item_record_id = a.item_record_id();
        s.item_guid = a.item_guid();
        s.item_rarity = a.item_rarity();
        s.item_quality = a.item_quality();
        s.combo_button = a.combo_button();
        r.character.action_bar.push_back(std::move(s));
    }
    // Nodes 1, stage 2: the account's other characters, for the node window's relationships (a failure leaves the list empty: the join lists this character alone)
    if (r.character.account_id != 0) {
        grpc::ClientContext list_ctx;
        prepare(list_ctx);
        characterv1::ListCharactersRequest list_req;
        list_req.set_account_id(r.character.account_id);
        characterv1::ListCharactersResponse list_resp;
        if (stubs_->character->ListCharacters(&list_ctx, list_req, &list_resp).ok()) {
            for (const auto& other : list_resp.characters()) r.character.account_characters.push_back({other.id(), other.name()});
        }
    }
    return r;
}

CallStatus GrpcServices::save_progress(const std::string& id, const Progress& progress) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::SaveProgressRequest req;
    req.set_id(id);
    req.set_level(progress.level);
    req.set_class_id(progress.class_id);
    for (const Experience& e : progress.experience) {
        auto* p = req.add_experience();
        p->set_leveling_type_guid(e.leveling_type_guid);
        p->set_xp(e.xp);
        p->set_debt(e.debt);
    }
    req.set_replace_skill_ranks(progress.replace_skill_ranks);
    for (const TreeRanks& t : progress.skill_ranks) {
        auto* p = req.add_skill_ranks();
        p->set_tree_guid(t.tree_guid);
        for (const int64_t r : t.rank_guids) p->add_rank_guids(r);
    }
    req.set_replace_action_bar(progress.replace_action_bar);
    for (const ActionbarSlot& a : progress.action_bar) {
        auto* p = req.add_action_bar();
        p->set_bank(a.bank);
        p->set_slot(a.slot);
        p->set_use_type(a.use_type);
        p->set_instigator(a.instigator);
        p->set_ability_id(a.ability_id);
        p->set_skill_rank_id(a.skill_rank_id);
        p->set_pet_command_id(a.pet_command_id);
        p->set_item_record_id(a.item_record_id);
        p->set_item_guid(a.item_guid);
        p->set_item_rarity(a.item_rarity);
        p->set_item_quality(a.item_quality);
        p->set_combo_button(a.combo_button);
    }
    characterv1::SaveProgressResponse resp;
    return map_grpc_status(stubs_->character->SaveProgress(&ctx, req, &resp));
}

CallStatus GrpcServices::save_position(const std::string& id, const Position& position) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::SaveCharacterStateRequest req;
    req.set_id(id);
    auto* p = req.mutable_position();   // last_region and level stay unset: the service leaves them unchanged
    p->set_x(position.x);
    p->set_y(position.y);
    p->set_z(position.z);
    p->set_yaw(position.yaw);
    characterv1::SaveCharacterStateResponse resp;
    return map_grpc_status(stubs_->character->SaveCharacterState(&ctx, req, &resp));
}

CallStatus GrpcServices::save_inventory(const std::string& id, const std::vector<InventoryItem>& items) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::SaveInventoryRequest req;
    req.set_id(id);
    for (const InventoryItem& it : items) {
        auto* p = req.add_items();
        switch (it.container) {
        case ItemContainer::Backpack: p->set_container(characterv1::ITEM_CONTAINER_BACKPACK); break;
        case ItemContainer::Equipment: p->set_container(characterv1::ITEM_CONTAINER_EQUIPMENT); break;
        case ItemContainer::Quest: p->set_container(characterv1::ITEM_CONTAINER_QUEST); break;
        }
        p->set_slot(it.slot);
        p->set_item_record_id(it.item_record_id);
        p->set_instance_guid(it.instance_guid);
        p->set_quantity(it.quantity);
        p->set_quality(it.quality);
        p->set_durability(it.durability);
        p->set_created_at_ms(it.created_at_ms);
        if (it.persisted_health) p->set_persisted_health(*it.persisted_health);
        if (it.persisted_mana) p->set_persisted_mana(*it.persisted_mana);
        p->set_lock_expiration(it.lock_expiration);
    }
    characterv1::SaveInventoryResponse resp;
    return map_grpc_status(stubs_->character->SaveInventory(&ctx, req, &resp));
}
// ~ Cyb - C O A ~

RegisterResult GrpcServices::register_server(const std::string& address,
                                             const std::vector<std::string>& regions) {
    grpc::ClientContext ctx;
    prepare(ctx);
    meshv1::RegisterRequest req;
    req.set_address(address);
    for (const auto& region : regions) req.add_regions(region);
    meshv1::RegisterResponse resp;
    RegisterResult r;
    r.status = map_grpc_status(stubs_->mesh->Register(&ctx, req, &resp));
    if (r.status == CallStatus::Ok) {
        r.server_id = resp.server_id();
        r.heartbeat_interval_ms = resp.heartbeat_interval_ms();
    }
    return r;
}

CallStatus GrpcServices::heartbeat(uint32_t server_id) {
    grpc::ClientContext ctx;
    prepare(ctx);
    meshv1::HeartbeatRequest req;
    req.set_server_id(server_id);
    meshv1::HeartbeatResponse resp;
    return map_grpc_status(stubs_->mesh->Heartbeat(&ctx, req, &resp));
}

}}} // namespace aoc::worldd::svc

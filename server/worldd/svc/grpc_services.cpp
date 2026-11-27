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

/// Social, step 9: a guild as GetCharacter and GetCharacterByName send it, mapped to worldd's own form.
Guild guild_of(const characterv1::Guild& g) {
    Guild out;
    out.id = g.id();
    out.name = g.name();
    out.version = g.version();
    for (const auto& r : g.ranks()) out.ranks.push_back({r.name(), r.permissions()});
    out.level = g.level();
    out.experience = g.experience();
    out.capacity = g.capacity();
    out.level_cap = g.level_cap();
    for (const auto& m : g.members()) {
        GuildMember row;
        row.character_id = m.character_id();
        row.name = m.name();
        row.rank = m.rank();
        row.level = m.level();
        row.class_id = m.class_id();
        row.joined_ms = m.joined_ms();
        row.last_online_ms = m.last_online_ms();
        out.members.push_back(std::move(row));
    }
    out.self_rank = g.self_rank();
    out.cooldown_until_ms = g.cooldown_until_ms();
    return out;
}

/// Mail (migration 0027): one attachment as the character service sends it, mapped to worldd's form. An attachment has no place in the bags yet: the container
/// and slot are the defaults and are not used until a claim moves the item.
MailItem mail_item_of(const characterv1::MailItem& i) {
    MailItem out;
    out.item.container = ItemContainer::Backpack;
    out.item.slot = 0;
    out.item.item_record_id = i.item_record_id();
    out.item.instance_guid = i.instance_guid();
    out.item.quantity = i.quantity();
    out.item.quality = i.quality();
    out.item.durability = i.durability();
    out.item.created_at_ms = i.created_at_ms();
    if (i.has_persisted_health()) out.item.persisted_health = i.persisted_health();
    if (i.has_persisted_mana()) out.item.persisted_mana = i.persisted_mana();
    out.item.lock_expiration = i.lock_expiration();
    out.claimed = i.claimed();
    return out;
}

/// Mail (migration 0027): a mail as the character service sends it, with its attachments in the mail's order.
Mail mail_of(const characterv1::Mail& m) {
    Mail out;
    out.id = m.id();
    out.sender = m.sender();
    out.subject = m.subject();
    out.body = m.body();
    out.status = m.status();
    out.issued_at_ms = m.issued_at_ms();
    out.expires_at_ms = m.expires_at_ms();
    for (const auto& i : m.items()) out.items.push_back(mail_item_of(i));
    return out;
}

/// Mail (migration 0027): one attachment as a save or a create sends it. Its container and slot stay unset: the attachment has no place until a claim moves it
/// into the bags.
void set_mail_item(characterv1::MailItem* p, const MailItem& it) {
    p->set_item_record_id(it.item.item_record_id);
    p->set_instance_guid(it.item.instance_guid);
    p->set_quantity(it.item.quantity);
    p->set_quality(it.item.quality);
    p->set_durability(it.item.durability);
    p->set_created_at_ms(it.item.created_at_ms);
    if (it.item.persisted_health) p->set_persisted_health(*it.item.persisted_health);
    if (it.item.persisted_mana) p->set_persisted_mana(*it.item.persisted_mana);
    p->set_lock_expiration(it.item.lock_expiration);
    p->set_claimed(it.claimed);
}

/// Mail (migration 0027): one mail as SaveMail sends it.
void set_mail(characterv1::Mail* m, const Mail& mail) {
    m->set_id(mail.id);
    m->set_sender(mail.sender);
    m->set_subject(mail.subject);
    m->set_body(mail.body);
    m->set_status(mail.status);
    m->set_issued_at_ms(mail.issued_at_ms);
    m->set_expires_at_ms(mail.expires_at_ms);
    for (const MailItem& it : mail.items) set_mail_item(m->add_items(), it);
}

/// Marketplace (migration 0030): one item as the bags send it.
void set_item(characterv1::Item* p, const InventoryItem& it) {
    switch (it.container) {
    case ItemContainer::Backpack: p->set_container(characterv1::ITEM_CONTAINER_BACKPACK); break;
    case ItemContainer::Equipment: p->set_container(characterv1::ITEM_CONTAINER_EQUIPMENT); break;
    case ItemContainer::Quest: p->set_container(characterv1::ITEM_CONTAINER_QUEST); break;
    case ItemContainer::Resource0: p->set_container(characterv1::ITEM_CONTAINER_RESOURCE0); break;
    case ItemContainer::Resource1: p->set_container(characterv1::ITEM_CONTAINER_RESOURCE1); break;
    case ItemContainer::Resource2: p->set_container(characterv1::ITEM_CONTAINER_RESOURCE2); break;
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

/// Marketplace: one listing as the save and the read carry it.
MarketListing market_listing_of(const characterv1::MarketListing& l) {
    MarketListing out;
    out.id = l.id();
    out.seller_id = l.seller_id();
    out.seller_name = l.seller_name();
    out.node = l.market_node();
    const characterv1::Item& item = l.item();
    out.item.item_record_id = item.item_record_id();
    out.item.instance_guid = item.instance_guid();
    out.item.quantity = item.quantity();
    out.item.quality = item.quality();
    out.item.durability = item.durability();
    out.item.created_at_ms = item.created_at_ms();
    if (item.has_persisted_health()) out.item.persisted_health = item.persisted_health();
    if (item.has_persisted_mana()) out.item.persisted_mana = item.persisted_mana();
    out.item.lock_expiration = item.lock_expiration();
    out.price_per_unit = l.price_per_unit();
    out.status = l.status();
    out.issue_date_ms = l.issue_date_ms();
    out.expiry_date_ms = l.expiry_date_ms();
    return out;
}

void set_market_listing(characterv1::MarketListing* p, const MarketListing& l) {
    p->set_id(l.id);
    p->set_market_node(l.node);
    set_item(p->mutable_item(), l.item);
    p->set_price_per_unit(l.price_per_unit);
    p->set_status(l.status);
    p->set_issue_date_ms(l.issue_date_ms);
    p->set_expiry_date_ms(l.expiry_date_ms);
}

}  // namespace

CallStatus map_grpc_status(const grpc::Status& s) {
    switch (s.error_code()) {
    case grpc::StatusCode::OK: return CallStatus::Ok;
    case grpc::StatusCode::PERMISSION_DENIED:
    case grpc::StatusCode::ALREADY_EXISTS:   // (a guild name another guild has)
    case grpc::StatusCode::INVALID_ARGUMENT: return CallStatus::Refused;
    case grpc::StatusCode::NOT_FOUND: return CallStatus::NotFound;
    case grpc::StatusCode::UNIMPLEMENTED: return CallStatus::Unsupported;   // (the service keeps none of this call)
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

CharacterResult GrpcServices::get_character_by_name(const std::string& name) {
    grpc::ClientContext ctx;
    prepare(ctx);
    characterv1::GetCharacterByNameRequest req;
    req.set_name(name);
    characterv1::GetCharacterByNameResponse resp;
    CharacterResult r;
    r.status = map_grpc_status(stubs_->character->GetCharacterByName(&ctx, req, &resp));
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
    r.character.level = c.level();
    if (c.has_guild()) r.character.guild = guild_of(c.guild());   // social, step 9
    return r;
}
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
    if (c.has_guild()) r.character.guild = guild_of(c.guild());   // social, step 9
    r.character.unread_mail_count = c.unread_mail_count();   // mail (migration 0027)
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
        case characterv1::ITEM_CONTAINER_RESOURCE0: it.container = ItemContainer::Resource0; break;
        case characterv1::ITEM_CONTAINER_RESOURCE1: it.container = ItemContainer::Resource1; break;
        case characterv1::ITEM_CONTAINER_RESOURCE2: it.container = ItemContainer::Resource2; break;
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
    for (const auto& w : c.wallet()) r.character.wallet.push_back({w.currency_guid(), w.amount()});
    for (const auto& j : c.processing_jobs()) {
        ProcessingJob job;
        job.job_guid = j.job_guid();
        job.station_key = j.station_key();
        job.recipe_guid = j.recipe_guid();
        job.sub_recipe_guid = j.sub_recipe_guid();
        job.quantity = j.quantity();
        job.queued_at_ms = j.queued_at_ms();
        job.started_at_ms = j.started_at_ms();
        job.duration_ms = j.duration_ms();
        job.client_txn = j.client_txn();
        job.rating_xp = j.rating_xp();
        job.rating_quantity = j.rating_quantity();
        job.rating_rarity = j.rating_rarity();
        for (const auto& res : j.resources()) {
            JobResource jr;
            jr.record_id = res.record_id();
            for (const auto& ra : res.rarities()) jr.rarities.push_back({ra.rarity(), ra.count(), ra.from_warehouse()});
            job.resources.push_back(std::move(jr));
        }
        r.character.processing_jobs.push_back(std::move(job));
    }
    for (const int64_t recipe : c.known_recipes()) r.character.known_recipes.push_back(recipe);
    for (const int64_t id : c.completed_ftues()) r.character.completed_ftues.push_back(id);
    for (const auto& w : c.warehouse_items()) {
        WarehouseItem wi;
        wi.area = w.area();
        wi.node_guid = w.node_guid();
        wi.container_guid = w.container_guid();
        wi.item.slot = w.slot();
        wi.item.item_record_id = w.item_record_id();
        wi.item.instance_guid = w.instance_guid();
        wi.item.quantity = w.quantity();
        wi.item.quality = w.quality();
        wi.item.durability = w.durability();
        wi.item.created_at_ms = w.created_at_ms();
        if (w.has_persisted_health()) wi.item.persisted_health = w.persisted_health();
        if (w.has_persisted_mana()) wi.item.persisted_mana = w.persisted_mana();
        wi.item.lock_expiration = w.lock_expiration();
        r.character.warehouse_items.push_back(std::move(wi));
    }
    for (const int32_t n : c.attunements()) r.character.attunements.push_back(n);
    r.character.home_attunement = c.home_attunement();
    r.character.home_last_used_ms = c.home_last_used_ms();
    for (const std::string& n : c.ignored_character_names()) r.character.ignored_character_names.push_back(n);
    for (const std::string& n : c.ignored_guild_names()) r.character.ignored_guild_names.push_back(n);
    for (const auto& f : c.friends()) {
        Friend x;
        x.guid = f.guid();
        x.name = f.name();
        x.level = f.level();
        x.class_id = f.class_id();
        x.sub_archetype = f.sub_archetype();
        x.guild_name = f.guild_name();
        x.still_friend = f.still_friend();
        r.character.friends.push_back(std::move(x));
    }
    for (const auto& q : c.friend_requests()) {
        FriendRequest x;
        x.sender_guid = q.sender_guid();
        x.sender_name = q.sender_name();
        x.recipient_guid = q.recipient_guid();
        x.expires_at_ms = q.expires_at_ms();
        r.character.friend_requests.push_back(std::move(x));
    }
    if (c.has_pvp()) {   // PvP (migration 0025)
        // PvP: contributed by MrJambix in #23.
        r.character.corruption = c.pvp().corruption();
        r.character.blight = c.pvp().blight();
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
    for (const auto& p : c.professions()) {
        ProfessionState x;
        x.profession_guid = p.profession_guid();
        x.certification_tier = p.certification_tier();
        x.level = p.level();
        x.xp = p.xp();
        r.character.professions.push_back(x);
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
// CoA and C:Y:B

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
    for (const ProfessionState& pr : progress.professions) {
        auto* p = req.add_professions();
        p->set_profession_guid(pr.profession_guid);
        p->set_certification_tier(pr.certification_tier);
        p->set_level(pr.level);
        p->set_xp(pr.xp);
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
    req.set_replace_completed_ftues(progress.replace_completed_ftues);
    for (const int64_t id : progress.completed_ftues) req.add_completed_ftues(id);
    req.set_replace_attunements(progress.replace_attunements);
    for (const int32_t n : progress.attunements) req.add_attunements(n);
    req.set_home_attunement(progress.home_attunement);
    req.set_home_last_used_ms(progress.home_last_used_ms);
    req.set_replace_ignored_names(progress.replace_ignored_names);
    for (const std::string& n : progress.ignored_character_names) req.add_ignored_character_names(n);
    for (const std::string& n : progress.ignored_guild_names) req.add_ignored_guild_names(n);
    req.set_replace_friends(progress.replace_friends);
    for (const Friend& f : progress.friends) {
        auto* p = req.add_friends();
        p->set_guid(f.guid);
        p->set_name(f.name);
        p->set_level(f.level);
        p->set_class_id(f.class_id);
        p->set_sub_archetype(f.sub_archetype);
        p->set_guild_name(f.guild_name);
        p->set_still_friend(f.still_friend);
    }
    req.set_replace_friend_requests(progress.replace_friend_requests);
    for (const FriendRequest& q : progress.friend_requests) {
        auto* p = req.add_friend_requests();
        p->set_sender_guid(q.sender_guid);
        p->set_sender_name(q.sender_name);
        p->set_recipient_guid(q.recipient_guid);
        p->set_expires_at_ms(q.expires_at_ms);
    }
    if (progress.replace_pvp) {   // PvP (migration 0025): both 0 sends no state (no row)
        // PvP: contributed by MrJambix in #23.
        req.set_replace_pvp(true);
        if (progress.corruption != 0 || progress.blight != 0) {
            req.mutable_pvp()->set_corruption(progress.corruption);
            req.mutable_pvp()->set_blight(progress.blight);
        }
    }
    characterv1::SaveProgressResponse resp;
    return map_grpc_status(stubs_->character->SaveProgress(&ctx, req, &resp));
}

CallStatus GrpcServices::save_guild(const Guild& guild) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::SaveGuildRequest req;
    auto* g = req.mutable_guild();
    g->set_id(guild.id);
    g->set_name(guild.name);
    g->set_version(guild.version);
    for (const GuildRank& r : guild.ranks) {
        auto* p = g->add_ranks();
        p->set_name(r.name);
        p->set_permissions(r.permissions);
    }
    g->set_level(guild.level);
    g->set_experience(guild.experience);
    g->set_capacity(guild.capacity);
    g->set_level_cap(guild.level_cap);
    for (const GuildMember& m : guild.members) {
        auto* p = g->add_members();
        p->set_character_id(m.character_id);
        p->set_name(m.name);
        p->set_rank(m.rank);
        p->set_joined_ms(m.joined_ms);
        p->set_last_online_ms(m.last_online_ms);
    }
    characterv1::SaveGuildResponse resp;
    req.set_disband(guild.disband);
    return map_grpc_status(stubs_->character->SaveGuild(&ctx, req, &resp));
}

CallStatus GrpcServices::save_guild_cooldown(const std::string& character_id, int64_t until_ms) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::SaveGuildCooldownRequest req;
    req.set_character_id(character_id);
    req.set_until_ms(until_ms);
    characterv1::SaveGuildCooldownResponse resp;
    return map_grpc_status(stubs_->character->SaveGuildCooldown(&ctx, req, &resp));
}

MailsResult GrpcServices::load_mails(const std::string& character_id) {
    grpc::ClientContext ctx;
    prepare(ctx);
    characterv1::GetMailsRequest req;
    req.set_character_id(character_id);
    characterv1::GetMailsResponse resp;
    const grpc::Status s = stubs_->character->GetMails(&ctx, req, &resp);
    MailsResult r;
    r.status = map_grpc_status(s);
    if (r.status != CallStatus::Ok) {
        r.reason = s.error_message();
        return r;
    }
    for (const auto& m : resp.mails()) r.mails.push_back(mail_of(m));
    return r;
}

MailSaveResult GrpcServices::save_mail(const std::string& character_id, const Mail& mail) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::SaveMailRequest req;
    req.set_character_id(character_id);
    set_mail(req.mutable_mail(), mail);
    characterv1::SaveMailResponse resp;
    const grpc::Status s = stubs_->character->SaveMail(&ctx, req, &resp);
    MailSaveResult r;
    r.status = map_grpc_status(s);
    if (r.status != CallStatus::Ok) r.reason = s.error_message();
    return r;
}

MailResult GrpcServices::create_mail(const std::string& character_id, const NewMail& mail) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::CreateMailRequest req;
    req.set_character_id(character_id);
    req.set_sender(mail.sender);
    req.set_subject(mail.subject);
    req.set_body(mail.body);
    req.set_expires_in_ms(mail.expires_in_ms);
    for (const MailItem& it : mail.items) set_mail_item(req.add_items(), it);
    characterv1::CreateMailResponse resp;
    const grpc::Status s = stubs_->character->CreateMail(&ctx, req, &resp);
    MailResult r;
    r.status = map_grpc_status(s);
    if (r.status != CallStatus::Ok) {
        r.reason = s.error_message();
        return r;
    }
    if (resp.has_mail()) r.mail = mail_of(resp.mail());
    return r;
}

MarketSaveResult GrpcServices::save_inventory_and_listing(const std::string& character_id, const std::vector<InventoryItem>& items,
                                                          const std::vector<WalletEntry>& wallet, const std::vector<ProcessingJob>& jobs,
                                                          const std::vector<int64_t>& known_recipes, const std::vector<WarehouseItem>& warehouse,
                                                          const MarketListing& listing) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::SaveInventoryAndListingRequest req;
    req.set_character_id(character_id);
    for (const InventoryItem& it : items) set_item(req.add_items(), it);
    for (const WalletEntry& e : wallet) {
        auto* w = req.add_wallet();
        w->set_currency_guid(e.currency_guid);
        w->set_amount(e.amount);
    }
    for (const ProcessingJob& job : jobs) {
        auto* j = req.add_processing_jobs();
        j->set_job_guid(job.job_guid);
        j->set_station_key(job.station_key);
        j->set_recipe_guid(job.recipe_guid);
        j->set_sub_recipe_guid(job.sub_recipe_guid);
        j->set_quantity(job.quantity);
        j->set_queued_at_ms(job.queued_at_ms);
        j->set_started_at_ms(job.started_at_ms);
        j->set_duration_ms(job.duration_ms);
        j->set_client_txn(job.client_txn);
        j->set_rating_xp(job.rating_xp);
        j->set_rating_quantity(job.rating_quantity);
        j->set_rating_rarity(job.rating_rarity);
        for (const JobResource& res : job.resources) {
            auto* r = j->add_resources();
            r->set_record_id(res.record_id);
            for (const JobRarity& ra : res.rarities) {
                auto* rr = r->add_rarities();
                rr->set_rarity(ra.rarity);
                rr->set_count(ra.count);
                rr->set_from_warehouse(ra.from_warehouse);
            }
        }
    }
    for (const int64_t recipe : known_recipes) req.add_known_recipes(recipe);
    for (const WarehouseItem& w : warehouse) {
        auto* p = req.add_warehouse_items();
        p->set_area(w.area);
        p->set_node_guid(w.node_guid);
        p->set_container_guid(w.container_guid);
        p->set_slot(w.item.slot);
        p->set_item_record_id(w.item.item_record_id);
        p->set_instance_guid(w.item.instance_guid);
        p->set_quantity(w.item.quantity);
        p->set_quality(w.item.quality);
        p->set_durability(w.item.durability);
        p->set_created_at_ms(w.item.created_at_ms);
        if (w.item.persisted_health) p->set_persisted_health(*w.item.persisted_health);
        if (w.item.persisted_mana) p->set_persisted_mana(*w.item.persisted_mana);
        p->set_lock_expiration(w.item.lock_expiration);
    }
    set_market_listing(req.mutable_listing(), listing);
    characterv1::SaveInventoryAndListingResponse resp;
    MarketSaveResult r;
    const grpc::Status s = stubs_->character->SaveInventoryAndListing(&ctx, req, &resp);
    r.status = map_grpc_status(s);
    if (s.error_code() == grpc::StatusCode::ALREADY_EXISTS) {
        // this call's own reading of AlreadyExists (the shared mapping keeps it a refusal): the service holds the listing already, so an earlier try of the same save
        // committed and this one wrote nothing
        r.status = CallStatus::Ok;
        r.already_saved = true;
    }
    if (r.status != CallStatus::Ok) r.reason = s.error_message();
    return r;
}

MarketListingsResult GrpcServices::list_market_listings() {
    grpc::ClientContext ctx;
    prepare(ctx);
    characterv1::ListMarketListingsRequest req;
    characterv1::ListMarketListingsResponse resp;
    MarketListingsResult r;
    const grpc::Status s = stubs_->character->ListMarketListings(&ctx, req, &resp);
    r.status = map_grpc_status(s);
    if (r.status != CallStatus::Ok) {
        r.reason = s.error_message();
        return r;
    }
    for (const auto& l : resp.listings()) r.listings.push_back(market_listing_of(l));
    return r;
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

CallStatus GrpcServices::save_inventory(const std::string& id, const std::vector<InventoryItem>& items) { return save_inventory(id, items, {}, /*with_wallet=*/false); }

CallStatus GrpcServices::save_inventory(const std::string& id, const std::vector<InventoryItem>& items, const std::vector<WalletEntry>& wallet) {
    return save_inventory(id, items, wallet, /*with_wallet=*/true);
}

CallStatus GrpcServices::save_inventory(const std::string& id, const std::vector<InventoryItem>& items, const std::vector<WalletEntry>& wallet, const std::vector<ProcessingJob>& jobs) {
    return save_inventory(id, items, wallet, /*with_wallet=*/true, &jobs);
}

CallStatus GrpcServices::save_inventory(const std::string& id, const std::vector<InventoryItem>& items, const std::vector<WalletEntry>& wallet, const std::vector<ProcessingJob>& jobs,
                                        const std::vector<int64_t>& known_recipes) {
    return save_inventory(id, items, wallet, /*with_wallet=*/true, &jobs, &known_recipes);
}

CallStatus GrpcServices::save_inventory(const std::string& id, const std::vector<InventoryItem>& items, const std::vector<WalletEntry>& wallet, const std::vector<ProcessingJob>& jobs,
                                        const std::vector<int64_t>& known_recipes, const std::vector<WarehouseItem>& warehouse) {
    return save_inventory(id, items, wallet, /*with_wallet=*/true, &jobs, &known_recipes, &warehouse);
}

namespace {

/// What SaveInventory and ClaimMailItems both send of the bags: the items and, when given, the wallet, the processing jobs, the known recipes and the warehouse, each replaced whole.
void fill_inventory_request(characterv1::SaveInventoryRequest& req, const std::vector<InventoryItem>& items, const std::vector<WalletEntry>& wallet, bool with_wallet,
                            const std::vector<ProcessingJob>* jobs, const std::vector<int64_t>* known_recipes, const std::vector<WarehouseItem>* warehouse) {
    if (warehouse) {
        req.set_replace_warehouse(true);
        for (const WarehouseItem& w : *warehouse) {
            auto* p = req.add_warehouse_items();
            p->set_area(w.area);
            p->set_node_guid(w.node_guid);
            p->set_container_guid(w.container_guid);
            p->set_slot(w.item.slot);
            p->set_item_record_id(w.item.item_record_id);
            p->set_instance_guid(w.item.instance_guid);
            p->set_quantity(w.item.quantity);
            p->set_quality(w.item.quality);
            p->set_durability(w.item.durability);
            p->set_created_at_ms(w.item.created_at_ms);
            if (w.item.persisted_health) p->set_persisted_health(*w.item.persisted_health);
            if (w.item.persisted_mana) p->set_persisted_mana(*w.item.persisted_mana);
            p->set_lock_expiration(w.item.lock_expiration);
        }
    }
    if (known_recipes) {
        req.set_replace_known_recipes(true);
        for (const int64_t recipe : *known_recipes) req.add_known_recipes(recipe);
    }
    if (jobs) {
        req.set_replace_processing_jobs(true);
        for (const ProcessingJob& job : *jobs) {
            auto* j = req.add_processing_jobs();
            j->set_job_guid(job.job_guid);
            j->set_station_key(job.station_key);
            j->set_recipe_guid(job.recipe_guid);
            j->set_sub_recipe_guid(job.sub_recipe_guid);
            j->set_quantity(job.quantity);
            j->set_queued_at_ms(job.queued_at_ms);
            j->set_started_at_ms(job.started_at_ms);
            j->set_duration_ms(job.duration_ms);
            j->set_client_txn(job.client_txn);
            j->set_rating_xp(job.rating_xp);
            j->set_rating_quantity(job.rating_quantity);
            j->set_rating_rarity(job.rating_rarity);
            for (const JobResource& res : job.resources) {
                auto* r = j->add_resources();
                r->set_record_id(res.record_id);
                for (const JobRarity& ra : res.rarities) {
                    auto* rr = r->add_rarities();
                    rr->set_rarity(ra.rarity);
                    rr->set_count(ra.count);
                    rr->set_from_warehouse(ra.from_warehouse);
                }
            }
        }
    }
    if (with_wallet) {
        req.set_replace_wallet(true);
        for (const WalletEntry& w : wallet) {
            auto* e = req.add_wallet();
            e->set_currency_guid(w.currency_guid);
            e->set_amount(w.amount);
        }
    }
    for (const InventoryItem& it : items) {
        auto* p = req.add_items();
        switch (it.container) {
        case ItemContainer::Backpack: p->set_container(characterv1::ITEM_CONTAINER_BACKPACK); break;
        case ItemContainer::Equipment: p->set_container(characterv1::ITEM_CONTAINER_EQUIPMENT); break;
        case ItemContainer::Quest: p->set_container(characterv1::ITEM_CONTAINER_QUEST); break;
        case ItemContainer::Resource0: p->set_container(characterv1::ITEM_CONTAINER_RESOURCE0); break;
        case ItemContainer::Resource1: p->set_container(characterv1::ITEM_CONTAINER_RESOURCE1); break;
        case ItemContainer::Resource2: p->set_container(characterv1::ITEM_CONTAINER_RESOURCE2); break;
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
}

}  // namespace

CallStatus GrpcServices::save_inventory(const std::string& id, const std::vector<InventoryItem>& items, const std::vector<WalletEntry>& wallet, bool with_wallet, const std::vector<ProcessingJob>* jobs,
                                        const std::vector<int64_t>* known_recipes, const std::vector<WarehouseItem>* warehouse) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::SaveInventoryRequest req;
    req.set_id(id);
    fill_inventory_request(req, items, wallet, with_wallet, jobs, known_recipes, warehouse);
    characterv1::SaveInventoryResponse resp;
    return map_grpc_status(stubs_->character->SaveInventory(&ctx, req, &resp));
}

MailClaimResult GrpcServices::claim_mail_items(const std::string& character_id, const std::vector<MailClaim>& claims, const std::vector<InventoryItem>& items,
                                               const std::vector<WalletEntry>& wallet, const std::vector<ProcessingJob>& jobs, const std::vector<int64_t>& known_recipes,
                                               const std::vector<WarehouseItem>& warehouse) {
    grpc::ClientContext ctx;
    prepare(ctx, save_timeout_);
    characterv1::ClaimMailItemsRequest req;
    req.set_character_id(character_id);
    for (const MailClaim& claim : claims) {
        auto* c = req.add_claims();
        c->set_mail_id(claim.mail_id);
        c->set_instance_guid(claim.instance_guid);
    }
    fill_inventory_request(*req.mutable_inventory(), items, wallet, /*with_wallet=*/true, &jobs, &known_recipes, &warehouse);
    characterv1::ClaimMailItemsResponse resp;
    const grpc::Status s = stubs_->character->ClaimMailItems(&ctx, req, &resp);
    MailClaimResult r;
    r.status = map_grpc_status(s);
    if (r.status != CallStatus::Ok) {
        r.reason = s.error_message();
        r.claimed_before = s.error_code() == grpc::StatusCode::ALREADY_EXISTS;
        return r;
    }
    r.claimed = resp.claimed();
    r.replayed = resp.replayed();
    return r;
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

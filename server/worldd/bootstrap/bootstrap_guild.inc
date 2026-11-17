// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/bootstrap_guild.inc  (social, step 9: the pawn's guild component)
//
//  The player character's GuildComponent (UIntrepidGuildComponent): the guild's name over the nameplate (GuildName), the roster the guild window draws
//  (GuildData: the guild's GeneralGuildInfo and its member rows), the character's own member row (GuildMemberData) and the join cooldown (GuildCooldown,
//  an FDateTime).  The component is a default subobject of the player character the client knows by name, so its export is its name and the checksum every
//  retail export of it carries (kChecksumGuild), and its first content block is what gives it its id.
//
//  A character with no guild carries the component all the same, with nothing in it: a native default subobject exists on every pawn, and the guild window
//  asks it for the create call.  The join itself is the retail login's, bit for bit (the pawn's open and its second state hold no guild component); the
//  component goes out in a chain of its own on the pawn's channel once the character has phased in (guild_open), with its export, and set_guild at the join keeps
//  the guild's values until then (as the friends' lists are kept for the SocialComponent).  A character in no guild and with no cooldown sends no value:
//  the properties stay the game's defaults, which the client holds already (an empty roster is never written until a guild has been).
//
//  The GuildMember alignment (kAlignmentGuildMember) goes on the AlignmentComponent with the guild's guid as its instance: the client makes two players
//  guildmates from it (the target's alignment against the viewer's own guild), and it is what colours a guildmate's name.  It is appended after the
//  instances the game's own container holds (Player, the flag's, Instigator, PVPSuppressed, and a duel's), as a retail mount's inherited container
//  carries a guild after them; a flag change keeps it (combat/pvp.cpp merge_container leaves the instances it does not own).  A member's login sends it
//  in the same chain as the component.
// ============================================================================

namespace {

/// FGuildData as the guild component carries it: the general info, the three ranks with their permissions and the member rows.
repl::GuildData guild_data_of(const GuildWire& g) {
    repl::GuildData d;
    d.info.name = g.name;
    d.info.guid = repl::GuidValue{g.guid[0], g.guid[1], g.guid[2], g.guid[3]};
    for (const GuildRankWire& rank : g.ranks) d.info.ranks.push_back({rank.name, rank.permissions});
    d.info.level = g.level;
    d.info.experience = g.experience;
    d.info.capacity = g.capacity;
    d.info.level_cap = g.level_cap;
    for (const GuildMemberWire& m : g.members) {
        repl::GuildMemberData row;
        row.name = m.name;
        row.character_guid = repl::GuidValue{m.guid[0], m.guid[1], m.guid[2], m.guid[3]};
        row.rank = m.rank;
        row.level = m.level;
        row.class_id = m.class_id;
        row.last_online = m.last_online;
        row.joined = m.joined;
        row.group = m.group;
        row.guild_guid = repl::GuidValue{m.guild_guid[0], m.guild_guid[1], m.guild_guid[2], m.guild_guid[3]};
        row.online = m.online;
        d.members.push_back(std::move(row));
    }
    return d;
}

/// FDateTime ticks (100 ns intervals since 0001-01-01) of an instant given as Unix milliseconds (0: the date's own zero).
int64_t guild_ticks(int64_t unix_ms) {
    constexpr int64_t kUnixEpochTicks = 621355968000000000ll;
    return unix_ms == 0 ? 0 : kUnixEpochTicks + unix_ms * 10000ll;
}

}  // namespace

bool PlayerBootstrap::set_guild(const GuildWire& guild) {
    guild_wire_ = guild;
    if (!ok_ || !guild_added_) return ok_;   // (kept for the component's open, as set_friends keeps its lists for the SocialComponent)
    const bool in_a_guild = guild.guid != std::array<uint32_t, 4>{};
    if (!in_a_guild && guild.cooldown_until == 0 && !guild_written_) return true;   // (a character that never had a guild or a cooldown: every property stays the game's default and nothing goes out for it)
    repl::Subobject* comp = component(setup_.guids.guild);
    if (!comp || !comp->obj.schema) return false;
    const schema::ClassSchema& cls = *comp->obj.schema;
    bool ok = true;
    const auto set = [&](std::string_view path, Value v) { ok = comp->obj.state.set(cls, path, std::move(v)) && ok; };
    if (in_a_guild || guild_written_) {   // (a character on a cooldown that never had a guild gets no empty roster; one that left a guild gets its guild cleared)
        guild_written_ = true;
        ok = comp->obj.state.set(cls, "GuildName", Value::str(guild.name)) && ok;
        ok = comp->obj.state.set(cls, "GuildData", Value(guild_data_of(guild))) && ok;
        set("GuildMemberData.PersistedGuildMemberData.CharacterName", Value::str(guild.self.name));
        set("GuildMemberData.PersistedGuildMemberData.CharacterGuid.A", Value::i64(static_cast<int32_t>(guild.self.guid[0])));
        set("GuildMemberData.PersistedGuildMemberData.CharacterGuid.B", Value::i64(static_cast<int32_t>(guild.self.guid[1])));
        set("GuildMemberData.PersistedGuildMemberData.CharacterGuid.C", Value::i64(static_cast<int32_t>(guild.self.guid[2])));
        set("GuildMemberData.PersistedGuildMemberData.CharacterGuid.D", Value::i64(static_cast<int32_t>(guild.self.guid[3])));
        set("GuildMemberData.PersistedGuildMemberData.RankIndex", Value::u64(guild.self.rank));
        set("GuildMemberData.PersistedGuildMemberData.CharacterLevel", Value::u64(guild.self.level));
        set("GuildMemberData.PersistedGuildMemberData.CharacterClass", Value::u64(guild.self.class_id));
        set("GuildMemberData.PersistedGuildMemberData.LastTimeOnlineRealTime", Value::u64(guild.self.last_online));
        set("GuildMemberData.PersistedGuildMemberData.TimeJoinedRealTime", Value::u64(guild.self.joined));
        set("GuildMemberData.PersistedGuildMemberData.AssignedGroupIndex", Value::i64(guild.self.group));
        set("GuildMemberData.GuildGuid.A", Value::i64(static_cast<int32_t>(guild.self.guild_guid[0])));
        set("GuildMemberData.GuildGuid.B", Value::i64(static_cast<int32_t>(guild.self.guild_guid[1])));
        set("GuildMemberData.GuildGuid.C", Value::i64(static_cast<int32_t>(guild.self.guild_guid[2])));
        set("GuildMemberData.GuildGuid.D", Value::i64(static_cast<int32_t>(guild.self.guild_guid[3])));
        set("GuildMemberData.bIsOnline", Value::boolean(guild.self.online));
    }
    ok = comp->obj.state.set(cls, "GuildCooldown", Value(repl::Ticks{guild_ticks(guild.cooldown_until)})) && ok;
    return ok;
}

std::optional<Send> PlayerBootstrap::guild_update() {
    if (!ok_ || !guild_added_) return std::nullopt;
    return update(Role::Pawn, "GuildComponent", pawn_, pawn_shadow_);
}

bool PlayerBootstrap::set_guild_alignment(const std::array<uint32_t, 4>& guild) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* al = component(setup_.guids.alignment);
    if (!al || !al->obj.schema || !al->obj.schema->command("Alignments")) return false;
    const repl::Value* held = al->obj.state.get(*al->obj.schema, "Alignments");
    const auto* now = held ? held->get_if<repl::AlignmentContainer>() : nullptr;
    if (!now) return false;
    repl::AlignmentContainer next = *now;
    next.instances.erase(std::remove_if(next.instances.begin(), next.instances.end(),
                                        [](const repl::AlignmentContainer::Instance& i) { return i.key == kAlignmentGuildMember; }),
                         next.instances.end());
    if (guild != std::array<uint32_t, 4>{}) {
        next.instances.push_back({kAlignmentGuildMember, {repl::GuidValue{guild[0], guild[1], guild[2], guild[3]}}});
    }
    return al->obj.state.set(*al->obj.schema, "Alignments", Value(std::move(next)));
}

bool PlayerBootstrap::add_guild_component() {
    if (!ok_) return false;
    if (guild_added_) return true;
    repl::Subobject guild = make_component(setup_.guids.guild, "GuildComponent", kChecksumGuild);
    if (!guild.obj.schema) return fail("no schema for the guild component (UIntrepidGuildComponent)");
    pawn_.subobjects.push_back(std::move(guild));
    guild_added_ = true;
    return set_guild(guild_wire_);
}

std::optional<Send> PlayerBootstrap::guild_open() {
    if (!ok_ || !second_state_added_) return std::nullopt;
    if (guild_added_) return guild_update();   // (opened already: what changed since)
    if (!add_guild_component()) return std::nullopt;
    if (guild_wire_.guid != std::array<uint32_t, 4>{} && !set_guild_alignment(guild_wire_.guid)) {
        fail("the guild's alignment does not fit the pawn");
        return std::nullopt;
    }
    return update(Role::Pawn, "GuildComponent open", pawn_, pawn_shadow_);
}

std::optional<Send> PlayerBootstrap::guild_reply(bool ok, uint64_t request_type, const std::string& message) {
    if (!ok_ || !guild_added_) return std::nullopt;
    const repl::Subobject* guild = pawn_.find(setup_.guids.guild);
    if (!guild || !guild->obj.schema) return std::nullopt;
    const char* name = ok ? "ClientGuildRequestSucceeded" : "ClientGuildRequestFailed";
    const schema::RpcSchema* rpc = guild->obj.schema->rpc(name);
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "RequestType", Value::u64(request_type)) || !repl::set_rpc_param(*rpc, params, "Message", Value::str(message))) return std::nullopt;
    if (ok && !repl::set_rpc_param(*rpc, params, "bChatMessageOnly", Value::boolean(false))) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = name;
    call.params = std::move(params);
    return finish(Role::Pawn, false, name, repl::call_rpc(pawn_, guild, name, call.params, {}, acks_));
}

std::optional<Send> PlayerBootstrap::guild_create_panel() {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientOpenCreateGuildPanel");
    if (!rpc) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientOpenCreateGuildPanel";   // (no parameters)
    return rpcs(Role::PlayerController, "ClientOpenCreateGuildPanel", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::guild_invite(const std::string& request_uuid, const std::string& source_name, const std::string& guild_name) {
    if (!ok_ || !guild_added_) return std::nullopt;
    const repl::Subobject* guild = pawn_.find(setup_.guids.guild);
    if (!guild || !guild->obj.schema) return std::nullopt;
    const schema::RpcSchema* rpc = guild->obj.schema->rpc("ClientInvitePlayerToGuild");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "GuildRequestUUID", Value::str(request_uuid)) || !repl::set_rpc_param(*rpc, params, "SourceCharacterName", Value::str(source_name)) ||
        !repl::set_rpc_param(*rpc, params, "InGuildName", Value::str(guild_name))) {
        return std::nullopt;
    }
    repl::RpcRequest call;
    call.rpc = "ClientInvitePlayerToGuild";
    call.params = std::move(params);
    return finish(Role::Pawn, false, "ClientInvitePlayerToGuild", repl::call_rpc(pawn_, guild, "ClientInvitePlayerToGuild", call.params, {}, acks_));
}

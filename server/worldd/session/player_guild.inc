// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_guild.inc  (social, step 9: the guilds)
//
//  A guild is the data's: three ranks (the defaults of Guild_Information_Base: Guild Leader, Officer, Member, with that record's 35 permissions), a starting
//  capacity of 50, an invitation of 60 s, a join cooldown of 432,000 s after leaving one, no creation cost (the data's GuildCost is 0 Money) and the leader's
//  leaving action: the next-highest-ranking member becomes the leader.  The guild is one state for this world (guild_book(), shared by the sessions) and one
//  record in the character service (migration 0026): it is read with the character at the login and written whole after every change, versioned so a second
//  writer cannot overwrite a change.
//
//  The client's calls come on the pawn's guild component (UIntrepidGuildComponent, the scope RpcScope::GuildComponent): create, invite, accept, decline, leave,
//  kick, disband, reassign a member's rank, assign the leader, get the data, the rank hierarchy (refused: the data's three ranks are fixed here), and guild
//  chat.  Every request the guild answers gets ClientGuildRequestSucceeded(RequestType, Message, bChatMessageOnly 0) or ClientGuildRequestFailed(RequestType,
//  Message) on the same component, with the client's EGuildRequestType value; a rank that may not make a request is refused with the game's own text ("You do
//  not have permissions to invite players to your guild." and the like).  The roster, the guild's name, the cooldown and the member's own row live in the
//  component's replicated properties (bootstrap/bootstrap_guild.inc); the nameplate reads the name over every pawn, the guild window the roster on the
//  member's own pawn, and the GuildMember alignment (the guild's GUID) makes guildmates green.
//
//  Guild chat: ServerSendMessageToGuild(Message, Category, Emblems) reaches the online members of the guild whose rank has bReadGuildChat, from the sender's
//  name and in the category the client sent; only a rank with bWriteGuildChat may send one, and the game's own text is sent back otherwise.  A line that is
//  empty or over 512 characters is refused as chat's are.
//
//  Not built: the perks and the guild's levels and experience (the client may buy them; no call is answered), guild storage, wars, the board, the calendar and
//  the patron guild hall: their systems are not here (the report says so).  The CreateGuild cost is 0 (the data), so no purse is read.
// ============================================================================
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

// The client's EGuildRequestType values the replies carry.
constexpr uint64_t kGuildCreateType = 1, kGuildInviteType = 2, kGuildKickType = 3, kGuildAcceptType = 4, kGuildLeaveType = 5, kGuildMembersType = 6,
                   kGuildDisbandType = 7, kGuildAssignLeaderType = 8, kGuildDeclineType = 9, kGuildRankHierarchyType = 14, kGuildMemberRankType = 15;

// The 35 permissions in the client's order (FGuildPermissionsSettings): the bits this file reads.
constexpr uint64_t kGuildPermInvite = 1ull << 0;
constexpr uint64_t kGuildPermKick = 1ull << 1;
constexpr uint64_t kGuildPermDisband = 1ull << 7;
constexpr uint64_t kGuildPermAssignRanks = 1ull << 12;
constexpr uint64_t kGuildPermReadChat = 1ull << 29;
constexpr uint64_t kGuildPermWriteChat = 1ull << 30;

// The data's Guild_Information_Base: the starting capacity, the invitation's length, the cooldown after leaving and the leader's rank.
constexpr size_t kGuildMaxMembers = 50;
constexpr std::chrono::seconds kGuildInviteLifetime{60};
constexpr int64_t kGuildCooldownMs = 432000ll * 1000;
constexpr int kGuildLeaderRank = 0;

/// The longest guild name the server accepts (the client checks its own range from the game's constants; see the report).
constexpr size_t kGuildNameMin = 3, kGuildNameMax = 20;

char guild_fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool guild_same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return guild_fold(x) == guild_fold(y); });
}

std::string guild_trim(const std::string& text) {
    size_t first = 0, last = text.size();
    while (first < last && (text[first] == ' ' || text[first] == '\t')) ++first;
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t')) --last;
    return text.substr(first, last - first);
}

/// The client's own name rule: ASCII letters with single spaces between the words, 3 to 20 characters; `why` says which rule failed.
bool guild_name_ok(const std::string& name, std::string& why) {
    if (name.size() < kGuildNameMin || name.size() > kGuildNameMax) {
        why = "A guild name is 3 to 20 letters.";
        return false;
    }
    size_t words = 0;
    size_t i = 0;
    while (i < name.size()) {
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z'))) {
            why = "A guild name is letters, with single spaces between the words.";
            return false;
        }
        while (i < name.size() && ((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z'))) ++i;
        ++words;
        if (i < name.size()) {
            if (name[i] != ' ' || i + 1 == name.size()) {
                why = "A guild name is letters, with single spaces between the words.";
                return false;
            }
            ++i;
        }
    }
    if (words == 0) {
        why = "A guild name is letters, with single spaces between the words.";
        return false;
    }
    return true;
}

std::string guild_hex(const std::array<uint32_t, 4>& guid) {
    char text[33];
    std::snprintf(text, sizeof text, "%08x%08x%08x%08x", guid[0], guid[1], guid[2], guid[3]);
    return text;
}

/// The guild creation scroll the character holds: the first item in the item bag whose use opens the create panel (item_use.activation
/// OpenGuildCreationMenu).  The item's data says it, not a record Guid; a create takes one scroll (on_guild_create).
std::optional<svc::InventoryItem> guild_creation_scroll(const PlayerSession& s, const content::ContentDb& db) {
    for (const svc::InventoryItem& item : s.inventory().items()) {
        if (item.container != svc::ItemContainer::Backpack) continue;
        std::string error;
        const auto use = db.item_use(item.item_record_id, error);
        if (use && use->activation == "OpenGuildCreationMenu") return item;
    }
    return std::nullopt;
}

std::array<uint32_t, 4> guild_guid_of(const PlayerSession& s) {
    const auto& g = s.bootstrap().setup().character_guid;
    return {g[0], g[1], g[2], g[3]};
}

std::array<uint32_t, 4> guild_mint_guid() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::array<uint32_t, 4> guid{};
    for (uint32_t& word : guid) word = static_cast<uint32_t>(rng());
    if (guid == std::array<uint32_t, 4>{}) guid[0] = 1;
    return guid;
}

std::string guild_shown(const std::string& text) {
    std::string out = text.substr(0, 80);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
    }
    if (text.size() > 80) out += "...";
    return out;
}

/// The name a call carries at `path`, empty when the string is not on the wire; nullopt when the call does not decode.
std::optional<std::string> guild_call_text(const repl::ClientRpc& rpc, const char* path) {
    if (!rpc.call.decoded) return std::nullopt;
    const repl::Value* value = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path);
    const std::string* text = value ? value->get_if<std::string>() : nullptr;
    return text ? *text : std::string();
}

/// The four int32 words of the FGuid at `prefix` of a client call; nullopt when the call does not decode.
std::optional<std::array<uint32_t, 4>> guild_call_guid(const repl::ClientRpc& rpc, const char* prefix) {
    if (!rpc.call.decoded) return std::nullopt;
    std::array<uint32_t, 4> words{};
    for (int i = 0; i < 4; ++i) {
        const std::string path = std::string(prefix) + "." + "ABCD"[i];
        const auto word = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path), 0);
        if (!word) return std::nullopt;
        words[static_cast<size_t>(i)] = static_cast<uint32_t>(*word);
    }
    return words;
}

/// The guilds of this process: one world's sessions share them (a name is looked up in the world's directory before it joins one).
std::vector<std::unique_ptr<PlayerSession::Guild>>& guild_book() {
    static std::vector<std::unique_ptr<PlayerSession::Guild>> guilds;
    return guilds;
}

PlayerSession::Guild* guild_by_id(const std::array<uint32_t, 4>& id) {
    for (const auto& g : guild_book()) {
        if (g->id == id) return g.get();
    }
    return nullptr;
}

PlayerSession::Guild* guild_by_name(const std::string& name) {
    for (const auto& g : guild_book()) {
        if (guild_same_name(g->name, name)) return g.get();
    }
    return nullptr;
}

void guild_erase(PlayerSession::Guild* g) {
    auto& book = guild_book();
    book.erase(std::remove_if(book.begin(), book.end(), [g](const std::unique_ptr<PlayerSession::Guild>& held) { return held.get() == g; }), book.end());
}

/// Whether `g` is still one of the guilds of this process: a session that was not told when its guild went away (a failed one is skipped by the roster's
/// lookups) must not read it.
bool guild_alive(const PlayerSession::Guild* g) {
    if (!g) return false;
    for (const auto& held : guild_book()) {
        if (held.get() == g) return true;
    }
    return false;
}

/// The invitation `g` has out for the character `guid`, or nullptr.
PlayerSession::GuildInvite* guild_invite_of(PlayerSession::Guild& g, const std::array<uint32_t, 4>& guid) {
    for (PlayerSession::GuildInvite& i : g.invites) {
        if (i.guid == guid) return &i;
    }
    return nullptr;
}

/// `g` forgets its invitation for the character `guid`.
void guild_drop_invite(PlayerSession::Guild& g, const std::array<uint32_t, 4>& guid) {
    g.invites.erase(std::remove_if(g.invites.begin(), g.invites.end(), [&](const PlayerSession::GuildInvite& i) { return i.guid == guid; }), g.invites.end());
}

/// The guild as the character service takes it: version 0 for a create, else the version it writes over.
svc::Guild guild_service_of(const PlayerSession::Guild& g, int64_t version) {
    svc::Guild out;
    out.id = guild_hex(g.id);
    out.name = g.name;
    out.version = version;
    for (const PlayerSession::GuildRankRow& r : g.ranks) out.ranks.push_back({r.name, r.permissions});
    out.level = g.level;
    out.experience = g.experience;
    out.capacity = g.capacity;
    out.level_cap = g.level_cap;
    for (const PlayerSession::GuildMemberRow& row : g.members) {
        svc::GuildMember m;
        m.character_id = guild_hex(row.guid);
        m.name = row.name;
        m.rank = row.rank;
        m.level = row.level;
        m.class_id = row.class_id;
        m.joined_ms = static_cast<int64_t>(row.joined);
        m.last_online_ms = static_cast<int64_t>(row.last_online);
        out.members.push_back(std::move(m));
    }
    return out;
}

/// The session whose character is `guid` now, or nullptr (a session that has joined, phased in or not).
PlayerSession* guild_session_of(const PlayerWorld* world, const std::array<uint32_t, 4>& guid) {
    PlayerSession::ChatEntry* e = PlayerSession::ChatEntry::joined(world, guid);
    return e ? e->self : nullptr;
}

/// The roster's row of `guid` in `g`, or nullptr.
PlayerSession::GuildMemberRow* guild_row_of(PlayerSession::Guild& g, const std::array<uint32_t, 4>& guid) {
    for (PlayerSession::GuildMemberRow& row : g.members) {
        if (row.guid == guid) return &row;
    }
    return nullptr;
}

/// The live level of a member: the session's when it is in this world, the stored row's otherwise.
int guild_member_level(const PlayerWorld* world, const PlayerSession::GuildMemberRow& row) {
    if (const PlayerSession* s = guild_session_of(world, row.guid)) return s->progress().level;
    return row.level;
}

/// The live class of a member (a byte, the client's ECharacterClass).
int guild_member_class(const PlayerWorld* world, const PlayerSession::GuildMemberRow& row) {
    if (const PlayerSession* s = guild_session_of(world, row.guid)) return s->class_id();
    return row.class_id;
}

bool guild_member_online(const PlayerWorld* world, const std::array<uint32_t, 4>& guid) { return guild_session_of(world, guid) != nullptr; }

}  // namespace

const std::vector<PlayerSession::GuildRankRow>& PlayerSession::guild_default_ranks() {
    // Guild_Information_Base.DefaultRanks: the leader's 35 permissions all set, the officer's 26 and the member's 8 (the record's own values).
    static const std::vector<GuildRankRow> ranks = {{"Guild Leader", 0x7ffffffffull}, {"Officer", 0x7ffcfc7full}, {"Member", 0x65006014ull}};
    return ranks;
}

PlayerSession::GuildMemberRow* PlayerSession::guild_member_of(Guild& g, const std::array<uint32_t, 4>& guid) const { return guild_row_of(g, guid); }

bool PlayerSession::guild_may(const Guild& g, uint64_t bit) const {
    if (guild_rank_ < 0 || static_cast<size_t>(guild_rank_) >= g.ranks.size()) return false;
    return (g.ranks[static_cast<size_t>(guild_rank_)].permissions & bit) != 0;
}

// ---- the join ---------------------------------------------------------------------------------------------------------------

void PlayerSession::init_guild(const svc::Character& character) {
    guild_ = nullptr;
    guild_rank_ = -1;
    guild_name_.clear();
    guild_cooldown_until_ = 0;
    guild_level_seen_ = progress().level;
    if (character.guild) {
        guild_cooldown_until_ = std::max<int64_t>(0, character.guild->cooldown_until_ms);   // (a cooldown with no guild is the record of a character that left)
        if (const auto words = bootstrap::character_guid_words(character.guild->id)) {
            const std::array<uint32_t, 4> id = *words;
            Guild* g = guild_by_id(id);
            if (!g) {
                auto made = std::make_unique<Guild>();
                made->id = id;
                made->name = character.guild->name;
                made->version = character.guild->version;
                for (const svc::GuildRank& r : character.guild->ranks) made->ranks.push_back({r.name, r.permissions});
                if (made->ranks.empty()) made->ranks = guild_default_ranks();
                made->level = character.guild->level;
                made->experience = character.guild->experience;
                made->capacity = character.guild->capacity;
                made->level_cap = character.guild->level_cap;
                for (const svc::GuildMember& m : character.guild->members) {
                    const auto member_id = bootstrap::character_guid_words(m.character_id);
                    if (!member_id) continue;
                    GuildMemberRow row;
                    row.guid = *member_id;
                    row.name = m.name;
                    row.rank = static_cast<uint8_t>(std::clamp(m.rank, 0, 2));
                    row.level = static_cast<uint8_t>(std::clamp(m.level, 1, 25));
                    row.class_id = static_cast<uint8_t>(std::clamp(m.class_id, 0, 8));
                    row.joined = static_cast<uint64_t>(std::max<int64_t>(0, m.joined_ms));
                    row.last_online = static_cast<uint64_t>(std::max<int64_t>(0, m.last_online_ms));
                    row.guild_guid = id;
                    made->members.push_back(std::move(row));
                }
                g = made.get();
                guild_book().push_back(std::move(made));
                const bool led = std::any_of(g->members.begin(), g->members.end(), [](const GuildMemberRow& row) { return row.rank == kGuildLeaderRank; });
                if (!led && !g->members.empty()) {   // (the leader's character was deleted: the data's RemovedLeaderAction, as when the leader leaves)
                    guild_promote_next(*g);
                    guild_save(*g, /*create=*/false);
                }
            }
            guild_ = g;
            guild_name_ = g->name;
            const GuildMemberRow* mine = guild_row_of(*g, guild_guid_of(*this));
            guild_rank_ = mine && mine->rank < g->ranks.size() ? static_cast<int>(mine->rank) : -1;   // (the book's row: it is newer than the login's read when another member is online)
            if (guild_rank_ < 0) {   // (the book's state is newer than the login's read: the session is not a member of it)
                guild_ = nullptr;
                guild_rank_ = -1;
                guild_name_.clear();
            } else {
                guild_level_seen_ = progress().level;
            }
        } else {
            spdlog::warn("[guild] {}: the character service sent a guild id that is not 32 hex digits: the character joins no guild", name());
        }
    }
    bootstrap::GuildWire wire;
    wire.cooldown_until = guild_cooldown_until_;
    if (guild_) {
        if (const GuildMemberRow* self = guild_row_of(*guild_, guild_guid_of(*this))) wire = guild_wire_of(*guild_, *self);
    }
    pb_.set_guild(wire);
    // (the component and the GuildMember alignment go out after the phase-in: open_guild)
    if (guild_) {
        spdlog::info("[guild] {}: joins its guild {} ({} member(s), rank {})", name(), guild_->name, guild_->members.size(), guild_rank_);
        guild_refresh_members(*guild_, "a member logged in");   // (the others' rosters show this member online)
    }
}

// ---- the pawn's state -------------------------------------------------------------------------------------------------------

bootstrap::GuildWire PlayerSession::guild_wire_of(const Guild& g, const GuildMemberRow& self) const {
    bootstrap::GuildWire wire;
    wire.guid = g.id;
    wire.name = g.name;
    wire.level = static_cast<uint8_t>(std::clamp(g.level, 1, 10));
    wire.experience = static_cast<uint32_t>(std::max(0, g.experience));
    wire.capacity = static_cast<uint16_t>(std::clamp(g.capacity, 1, 200));
    wire.level_cap = static_cast<uint8_t>(std::clamp(g.level_cap, 1, 50));
    for (const GuildRankRow& r : g.ranks) wire.ranks.push_back({r.name, r.permissions});
    const auto row_of = [&](const GuildMemberRow& row, bool online) {
        bootstrap::GuildMemberWire m;
        m.guid = row.guid;
        m.name = row.name;
        m.rank = row.rank;
        m.level = static_cast<uint8_t>(std::clamp(guild_member_level(world_, row), 1, 25));
        m.class_id = static_cast<uint8_t>(std::clamp(guild_member_class(world_, row), 0, 8));
        m.last_online = row.last_online / 1000;   // (the store's milliseconds, the wire's Unix seconds)
        m.joined = row.joined / 1000;             // (the store's milliseconds, the wire's Unix seconds)
        m.group = row.group;
        m.guild_guid = g.id;
        m.online = online;
        return m;
    };
    for (const GuildMemberRow& row : g.members) wire.members.push_back(row_of(row, guild_member_online(world_, row.guid)));
    wire.self = row_of(self, guild_member_online(world_, self.guid));
    wire.cooldown_until = guild_cooldown_until_;
    return wire;
}

void PlayerSession::open_guild(Connection& conn) {
    queue("GuildComponent open", [this](bootstrap::PlayerBootstrap& pb) {
        std::optional<bootstrap::Send> open = pb.guild_open();
        if (guild_) note_shown_alignment();   // (the GuildMember alignment came with it: the pawn the others are shown takes it from there)
        return open;
    }, conn, /*must_build=*/false);   // (a member's roster may have opened the component before this step: then there is nothing left to send)
}

bool PlayerSession::guild_refresh_pawn(Connection& conn) {
    bootstrap::GuildWire wire;
    wire.cooldown_until = guild_cooldown_until_;
    const GuildMemberRow* self = guild_ ? guild_row_of(*guild_, guild_guid_of(*this)) : nullptr;
    if (guild_ && self) wire = guild_wire_of(*guild_, *self);
    if (!pb_.set_guild(wire)) {
        spdlog::warn("[guild] {}: the pawn's GuildComponent does not take the guild: {}", name(), pb_.error().empty() ? "no reason" : pb_.error());
        return false;
    }
    queue("GuildComponent", [](bootstrap::PlayerBootstrap& pb) { return pb.guild_open(); }, conn, /*must_build=*/true);   // (the first chain of the component when it has none yet)
    return true;
}

void PlayerSession::guild_refresh_members(Guild& g, const char* why) {
    size_t told = 0;
    for (const GuildMemberRow& row : g.members) {
        PlayerSession* member = guild_session_of(world_, row.guid);
        if (!member || member->guild_ != &g || member->finished_ || member->failed_) continue;
        Connection* conn = member->chat_entry_.connection;
        if (!conn) continue;
        member->guild_level_seen_ = member->progress().level;
        if (member->guild_refresh_pawn(*conn)) ++told;
    }
    spdlog::log(level_for(guild_logged_, 20), "[guild] {}: the guild {} was sent to {} member(s) ({})", name(), g.name, told, why);
}

// ---- the character service ---------------------------------------------------------------------------------------------------

void PlayerSession::guild_save(Guild& g, bool create) {
    if (!sink_) return;
    const svc::Guild out = guild_service_of(g, create ? 0 : g.version);
    g.version = create ? 1 : g.version + 1;   // (the save makes the version one more; the next save writes over it; the saver keeps the order of the saves of one guild)
    sink_->save_guild(out);
}

bool PlayerSession::guild_register(Guild& g) {
    if (!sink_) return true;
    const svc::CallStatus status = sink_->create_guild_now(guild_service_of(g, 0));
    if (status == svc::CallStatus::Ok) {
        g.version = 1;   // (the service holds it: nothing is left to create)
        return true;
    }
    if (status == svc::CallStatus::Refused) {
        spdlog::warn("[guild] {}: the character service refused the new guild {}: its name is taken", name(), guild_shown(g.name));
        return false;
    }
    guild_save(g, /*create=*/true);   // (the service did not answer: the guild is made here and saved as the service comes back)
    return true;
}

std::unique_ptr<PlayerSession::Guild> PlayerSession::guild_found(const std::string& name, const PlayerSession& founder, int64_t now_ms) {
    auto made = std::make_unique<Guild>();
    made->id = guild_mint_guid();
    made->name = name;
    made->ranks = guild_default_ranks();
    made->capacity = static_cast<int>(kGuildMaxMembers);
    GuildMemberRow row;
    row.guid = guild_guid_of(founder);
    row.name = founder.name();
    row.rank = static_cast<uint8_t>(kGuildLeaderRank);
    row.level = static_cast<uint8_t>(std::clamp(founder.progress().level, 1, 25));
    row.class_id = static_cast<uint8_t>(std::clamp(founder.class_id(), 0, 8));
    row.joined = static_cast<uint64_t>(std::max<int64_t>(0, now_ms));
    row.last_online = row.joined;
    row.guild_guid = made->id;
    made->members.push_back(std::move(row));
    return made;
}

void PlayerSession::guild_save_cooldown(const std::array<uint32_t, 4>& guid, int64_t until_ms) {
    if (!sink_) return;
    sink_->save_guild_cooldown(guild_hex(guid), until_ms);
}

void PlayerSession::guild_save_disbanded(Guild& g) {
    if (!sink_) return;
    svc::Guild out;
    out.id = guild_hex(g.id);
    out.name = g.name;
    out.version = g.version;
    out.disband = true;
    sink_->save_guild(out);
}

// ---- the membership ----------------------------------------------------------------------------------------------------------

void PlayerSession::guild_add_member(Guild& g, PlayerSession& who, int rank, int64_t now_ms) {
    if (GuildMemberRow* held = guild_row_of(g, guild_guid_of(who))) {
        held->rank = static_cast<uint8_t>(std::clamp(rank, 0, 2));
        held->last_online = static_cast<uint64_t>(now_ms);
    } else {
        GuildMemberRow row;
        row.guid = guild_guid_of(who);
        row.name = who.name();
        row.rank = static_cast<uint8_t>(std::clamp(rank, 0, 2));
        row.level = static_cast<uint8_t>(std::clamp(who.progress().level, 1, 25));
        row.class_id = static_cast<uint8_t>(std::clamp(who.class_id(), 0, 8));
        row.joined = static_cast<uint64_t>(std::max<int64_t>(0, now_ms));
        row.last_online = row.joined;
        row.guild_guid = g.id;
        g.members.push_back(std::move(row));
    }
    who.guild_ = &g;
    who.guild_rank_ = std::clamp(rank, 0, 2);
    who.guild_name_ = g.name;
    who.guild_cooldown_until_ = 0;
    who.guild_level_seen_ = who.progress().level;
    who.guild_pending_name_.clear();
    who.guild_pending_inviter_.clear();
    guild_drop_invite(g, guild_guid_of(who));
}

void PlayerSession::guild_promote_next(Guild& g) {
    GuildMemberRow* best = nullptr;
    for (GuildMemberRow& row : g.members) {
        if (row.rank == 0) continue;
        // the highest rank below the leader, then the earliest joined
        if (!best || row.rank < best->rank || (row.rank == best->rank && row.joined < best->joined)) best = &row;
    }
    if (!best) return;
    best->rank = 0;
    if (PlayerSession* s = guild_session_of(world_, best->guid); s && s->guild_ == &g) s->guild_rank_ = 0;
    spdlog::info("[guild] {}: {} is its leader now (the leader left)", name(), best->name);
}

void PlayerSession::guild_remove_member(Guild& g, const std::array<uint32_t, 4>& guid, bool tell, const char* why) {
    const auto at = std::find_if(g.members.begin(), g.members.end(), [&](const GuildMemberRow& row) { return row.guid == guid; });
    if (at == g.members.end()) return;
    const bool was_leader = at->rank == 0;
    const std::string name_of = at->name;
    PlayerSession* member = guild_session_of(world_, guid);
    g.members.erase(at);
    if (member) {
        member->guild_ = nullptr;
        member->guild_rank_ = -1;
        member->guild_name_.clear();
        member->guild_level_seen_ = 0;
        if (tell) {
            if (Connection* conn = member->chat_entry_.connection) member->guild_refresh_pawn(*conn);
        }
    }
    if (was_leader && !g.members.empty()) guild_promote_next(g);
    guild_refresh_members(g, why);
    spdlog::log(level_for(guild_logged_, 20), "[guild] {}: {} left the guild ({} member(s) remain)", name(), guild_shown(name_of), g.members.size());
}

void PlayerSession::guild_cancel_pending() {
    if (guild_pending_name_.empty()) return;
    if (Guild* g = guild_by_name(guild_pending_name_)) guild_drop_invite(*g, guild_guid_of(*this));
    guild_pending_name_.clear();
    guild_pending_inviter_.clear();
}

// ---- the calls --------------------------------------------------------------------------------------------------------------

void PlayerSession::on_guild_create(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    const std::optional<std::string> sent = guild_call_text(rpc, "InGuildName");   // (the client's own name for the name a create carries)
    if (!sent) {
        if (++guild_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[guild] {}: a ServerCreateGuild does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    if (guild_) {
        guild_reply(conn, false, kGuildCreateType, "You are already in a guild.");
        return;
    }
    if (trade_commit_pending()) {
        guild_reply(conn, false, kGuildCreateType, "Cannot create a guild while a trade is finalizing.");
        return;
    }
    if (guild_cooldown_until_ > wall_ms()) {
        guild_reply(conn, false, kGuildCreateType, "You may not join a guild for a while.");
        return;
    }
    const std::string name_wanted = guild_trim(*sent);
    std::string why;
    if (!guild_name_ok(name_wanted, why)) {
        guild_reply(conn, false, kGuildCreateType, why);
        return;
    }
    if (guild_by_name(name_wanted)) {
        guild_reply(conn, false, kGuildCreateType, "That guild name is taken.");
        return;
    }
    // the guild is made with a guild creation scroll (the item_use activation OpenGuildCreationMenu whose use opened the panel): a create without one is
    // refused, and every refusal above and below (the join cooldown, a name the service has) leaves the scroll in the bag
    const content::ContentDb* content = world_ ? world_->content() : nullptr;
    const std::optional<svc::InventoryItem> scroll = content ? guild_creation_scroll(*this, *content) : std::nullopt;
    if (!scroll) {
        guild_reply(conn, false, kGuildCreateType, "You need a guild creation scroll to create a guild.");
        return;
    }
    auto made = guild_found(name_wanted, *this, wall_ms());
    if (!guild_register(*made)) {   // (a guild none of whose members is online here has the name)
        guild_reply(conn, false, kGuildCreateType, "That guild name is taken.");
        return;
    }
    Guild* g = made.get();
    guild_book().push_back(std::move(made));
    guild_add_member(*g, *this, kGuildLeaderRank, wall_ms());
    // the scroll pays for the guild: one is taken now, as a used item's Remove (the use only opened the panel)
    const items::Outcome paid = inventory_.consume(scroll->container, scroll->slot, scroll->instance_guid, 1);
    if (paid.ok) {
        apply_outcome(conn, paid, "a guild creation scroll is used", /*with_toasts=*/true);
    } else {
        spdlog::warn("[guild] {}: the guild {} was made but its scroll stayed: {}", name(), g->name, paid.refusal);
    }
    guild_reply(conn, true, kGuildCreateType, "Guild " + g->name + " was successfully created!");
    guild_refresh_pawn(conn);
    pb_.set_guild_alignment(g->id);
    note_shown_alignment();
    spdlog::info("[guild] {}: created the guild {} ({})", name(), g->name, guild_hex(g->id));
}

void PlayerSession::on_guild_invite(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!guild_) {
        guild_reply(conn, false, kGuildInviteType, "You are not in a guild.");
        return;
    }
    if (!guild_may(*guild_, kGuildPermInvite)) {
        guild_reply(conn, false, kGuildInviteType, "You do not have permissions to invite players to your guild.");
        return;
    }
    const std::optional<std::string> sent = guild_call_text(rpc, "InviteeName");   // (the client's own name for the invited character's name)
    if (!sent) {
        if (++guild_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[guild] {}: a ServerInvitePlayerToGuild does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    const std::string target_name = guild_trim(*sent);
    if (target_name.empty()) {
        guild_reply(conn, false, kGuildInviteType, "No player is named.");
        return;
    }
    ChatEntry* entry = ChatEntry::find(world_, target_name);
    if (!entry || !entry->self) {
        guild_reply(conn, false, kGuildInviteType, target_name + " is not online.");
        return;
    }
    PlayerSession& target = *entry->self;
    if (target.guild_) {
        guild_reply(conn, false, kGuildInviteType, target.name() + " is already in a guild.");
        return;
    }
    if (target.guild_cooldown_until_ > wall_ms()) {
        guild_reply(conn, false, kGuildInviteType, target.name() + " may not join a guild for a while.");
        return;
    }
    if (guild_->members.size() >= static_cast<size_t>(std::max(1, guild_->capacity))) {
        guild_reply(conn, false, kGuildInviteType, "Your guild is full.");
        return;
    }
    if (!target.guild_pending_name_.empty()) {
        guild_reply(conn, false, kGuildInviteType, target.name() + " already has a guild invitation pending.");
        return;
    }
    Connection* target_conn = target.chat_entry_.connection;
    if (!target_conn) {
        guild_reply(conn, false, kGuildInviteType, target.name() + " is not online.");
        return;
    }
    guild_->invites.erase(std::remove_if(guild_->invites.begin(), guild_->invites.end(), [now](const GuildInvite& i) { return now >= i.expires; }), guild_->invites.end());
    guild_drop_invite(*guild_, guild_guid_of(target));
    guild_->invites.push_back({target.name(), guild_guid_of(target), name(), now + kGuildInviteLifetime});
    target.guild_pending_name_ = guild_->name;
    target.guild_pending_inviter_ = name();
    target.guild_pending_expires_ = now + kGuildInviteLifetime;
    const std::string uuid = guild_hex(guild_mint_guid());
    const std::string guild_name = guild_->name;
    const std::string inviter = name();
    target.queue("ClientInvitePlayerToGuild", [uuid, inviter, guild_name](bootstrap::PlayerBootstrap& pb) { return pb.guild_invite(uuid, inviter, guild_name); }, *target_conn,
                 /*must_build=*/true);
    guild_reply(conn, true, kGuildInviteType, target.name() + " has been invited to " + guild_->name + "!");
    spdlog::log(level_for(guild_logged_, 20), "[guild] {}: invites {} to {}", name(), target.name(), guild_->name);
}

void PlayerSession::on_guild_accept(Connection& conn, Clock::time_point now) {
    if (guild_) {
        guild_cancel_pending();
        guild_reply(conn, false, kGuildAcceptType, "You are already in a guild.");
        return;
    }
    if (guild_pending_name_.empty()) {
        guild_reply(conn, false, kGuildAcceptType, "No guild invitation waits for you.");
        return;
    }
    if (now >= guild_pending_expires_) {
        guild_cancel_pending();
        guild_reply(conn, false, kGuildAcceptType, "That invitation has expired.");
        return;
    }
    Guild* g = guild_by_name(guild_pending_name_);
    if (!g) {
        guild_pending_name_.clear();
        guild_pending_inviter_.clear();
        guild_reply(conn, false, kGuildAcceptType, "That guild no longer exists.");
        return;
    }
    if (!guild_invite_of(*g, guild_guid_of(*this))) {
        guild_pending_name_.clear();
        guild_pending_inviter_.clear();
        guild_reply(conn, false, kGuildAcceptType, "That invitation is no longer valid.");
        return;
    }
    if (g->members.size() >= static_cast<size_t>(std::max(1, g->capacity))) {
        guild_reply(conn, false, kGuildAcceptType, "That guild is full.");
        return;
    }
    if (guild_cooldown_until_ > wall_ms()) {
        guild_reply(conn, false, kGuildAcceptType, "You may not join a guild for a while.");
        return;
    }
    guild_add_member(*g, *this, 2, wall_ms());
    guild_save(*g, /*create=*/false);
    guild_reply(conn, true, kGuildAcceptType, name() + " has joined " + g->name + " Guild!");
    guild_refresh_pawn(conn);
    pb_.set_guild_alignment(g->id);
    note_shown_alignment();
    guild_refresh_members(*g, "a member joined");
    spdlog::info("[guild] {}: joined {} ({} member(s))", name(), g->name, g->members.size());
}

void PlayerSession::on_guild_decline(Connection& conn, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (guild_pending_name_.empty()) return;
    spdlog::log(level_for(guild_logged_, 20), "[guild] {}: declined the invitation of {}", name(), guild_shown(guild_pending_name_));
    guild_cancel_pending();
}

void PlayerSession::on_guild_leave(Connection& conn, Clock::time_point now) {
    (void)now;
    if (!guild_) {
        guild_reply(conn, false, kGuildLeaveType, "You are not in a guild.");
        return;
    }
    Guild* g = guild_;
    const std::string guild_name = g->name;
    const bool last = g->members.size() <= 1;
    guild_cooldown_until_ = wall_ms() + kGuildCooldownMs;
    guild_save_cooldown(guild_guid_of(*this), guild_cooldown_until_);
    guild_remove_member(*g, guild_guid_of(*this), /*tell=*/true, "a member left the guild");
    if (last) {
        guild_save_disbanded(*g);
        guild_erase(g);
    } else {
        guild_save(*g, /*create=*/false);
    }
    pb_.set_guild_alignment({});
    note_shown_alignment();
    guild_reply(conn, true, kGuildLeaveType, "You left the guild!");
    guild_refresh_pawn(conn);
    spdlog::info("[guild] {}: left {}", name(), guild_name);
}

void PlayerSession::on_guild_kick(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    if (!guild_) {
        guild_reply(conn, false, kGuildKickType, "You are not in a guild.");
        return;
    }
    if (!guild_may(*guild_, kGuildPermKick)) {
        guild_reply(conn, false, kGuildKickType, "You do not have permissions to kick players from the guild.");
        return;
    }
    const std::optional<std::array<uint32_t, 4>> kicked = guild_call_guid(rpc, "MemberGuidToKick");
    if (!kicked) {
        if (++guild_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[guild] {}: a ServerKickPlayerFromGuild does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    GuildMemberRow* row = guild_member_of(*guild_, *kicked);
    if (!row) {
        guild_reply(conn, false, kGuildKickType, "That player is not in your guild.");
        return;
    }
    if (row->guid == guild_guid_of(*this)) {
        guild_reply(conn, false, kGuildKickType, "You cannot kick yourself.");
        return;
    }
    if (guild_rank_ != kGuildLeaderRank && row->rank <= guild_rank_) {
        guild_reply(conn, false, kGuildKickType, "You cannot kick a member of your rank or above.");
        return;
    }
    const std::array<uint32_t, 4> guid = row->guid;
    const std::string removed = row->name;
    const std::string guild_name = guild_->name;
    if (PlayerSession* target = guild_session_of(world_, guid)) {
        target->guild_cooldown_until_ = wall_ms() + kGuildCooldownMs;
        target->guild_save_cooldown(guid, target->guild_cooldown_until_);
        if (Connection* target_conn = target->chat_entry_.connection) {
            target->pb_.set_guild_alignment({});
            target->note_shown_alignment();
            target->receive_chat_line(*target_conn, "You were kicked from the guild!", 124, "Server");
        }
    } else {
        guild_save_cooldown(guid, wall_ms() + kGuildCooldownMs);
    }
    guild_remove_member(*guild_, guid, /*tell=*/true, "a member was kicked");
    if (guild_->members.empty()) {
        guild_save_disbanded(*guild_);
        guild_erase(guild_);
        guild_ = nullptr;
        guild_rank_ = -1;
        guild_name_.clear();
        guild_refresh_pawn(conn);
        pb_.set_guild_alignment({});
        note_shown_alignment();
    } else {
        guild_save(*guild_, /*create=*/false);
    }
    guild_reply(conn, true, kGuildKickType, removed + " was kicked from the guild.");
    spdlog::log(level_for(guild_logged_, 20), "[guild] {}: kicks {} from {}", name(), guild_shown(removed), guild_name);
}

void PlayerSession::on_guild_disband(Connection& conn, Clock::time_point now) {
    (void)now;
    if (!guild_) {
        guild_reply(conn, false, kGuildDisbandType, "You are not in a guild.");
        return;
    }
    if (!guild_may(*guild_, kGuildPermDisband)) {
        guild_reply(conn, false, kGuildDisbandType, "You do not have permissions to demote a player.");   // (the game's own text for this refusal, word for word)
        return;
    }
    Guild* g = guild_;
    const std::string guild_name = g->name;
    const std::vector<GuildMemberRow> members = g->members;
    for (const GuildMemberRow& row : members) {
        PlayerSession* member = guild_session_of(world_, row.guid);
        if (!member) {
            guild_save_cooldown(row.guid, wall_ms() + kGuildCooldownMs);
            continue;
        }
        member->guild_ = nullptr;
        member->guild_rank_ = -1;
        member->guild_name_.clear();
        member->guild_level_seen_ = 0;
        member->guild_cooldown_until_ = wall_ms() + kGuildCooldownMs;
        member->guild_save_cooldown(row.guid, member->guild_cooldown_until_);
        if (Connection* member_conn = member->chat_entry_.connection) {
            member->pb_.set_guild_alignment({});
            member->note_shown_alignment();
            member->guild_refresh_pawn(*member_conn);
            if (member != this) member->receive_chat_line(*member_conn, name() + " disbanded the guild!", 124, "Server");
        }
    }
    g->members.clear();
    if (sink_) {
        svc::Guild out;
        out.id = guild_hex(g->id);
        out.name = g->name;
        out.version = g->version;
        out.capacity = g->capacity;
        out.level = g->level;
        out.level_cap = g->level_cap;
        out.disband = true;
        sink_->save_guild(out);
    }
    guild_erase(g);
    guild_ = nullptr;
    guild_rank_ = -1;
    guild_name_.clear();
    guild_reply(conn, true, kGuildDisbandType, name() + " disbanded the guild!");
    guild_refresh_pawn(conn);
    pb_.set_guild_alignment({});
    note_shown_alignment();
    spdlog::info("[guild] {}: disbanded {} ({} member(s))", name(), guild_name, members.size());
}

void PlayerSession::on_guild_reassign(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    if (!guild_) {
        guild_reply(conn, false, kGuildMemberRankType, "You are not in a guild.");
        return;
    }
    if (!guild_may(*guild_, kGuildPermAssignRanks)) {
        // the game has one text for a promotion and one for a demotion: the first member of the request says which this is
        bool demotion = false;
        const repl::Value* asked = rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "MemberRanksToUpdate") : nullptr;
        const repl::ArrayValue* asked_list = asked ? asked->get_if<repl::ArrayValue>() : nullptr;
        if (asked_list && !asked_list->elements.empty()) {
            const repl::ElementState& first = asked_list->elements.front();
            std::array<uint32_t, 4> who{};
            bool whole = true;
            for (uint16_t i = 1; i <= 4 && whole; ++i) {
                const auto it = first.values.find(i);
                const auto word = it == first.values.end() ? std::nullopt : repl::rpcf::int_or(&it->second, 0);
                whole = word.has_value();
                if (word) who[i - 1] = static_cast<uint32_t>(*word);
            }
            const auto rank_it = first.values.find(5);
            const auto wanted = rank_it == first.values.end() ? std::nullopt : repl::rpcf::uint_or(&rank_it->second, 0);
            const GuildMemberRow* now_row = whole ? guild_member_of(*guild_, who) : nullptr;
            demotion = now_row && wanted && *wanted > now_row->rank;
        }
        guild_reply(conn, false, kGuildMemberRankType,
                    demotion ? "You do not have permissions to demote a player." : "You do not have permissions to promote a player.");
        return;
    }
    const repl::Value* list_value = rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "MemberRanksToUpdate") : nullptr;
    const repl::ArrayValue* list = list_value ? list_value->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.empty() || list->elements.size() > 64) {
        if (++guild_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[guild] {}: a ServerReassignMemberRanks does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    struct Change {
        GuildMemberRow* row = nullptr;
        int             rank = 0;
    };
    std::vector<Change> changes;
    for (const repl::ElementState& e : list->elements) {
        std::array<uint32_t, 4> guid{};
        for (uint16_t i = 1; i <= 4; ++i) {
            const auto it = e.values.find(i);
            const auto v = it == e.values.end() ? std::nullopt : repl::rpcf::int_or(&it->second, 0);
            if (!v) {
                guild_reply(conn, false, kGuildMemberRankType, "A rank change does not name its member.");
                return;
            }
            guid[i - 1] = static_cast<uint32_t>(*v);
        }
        const auto it = e.values.find(5);
        const auto rank = it == e.values.end() ? std::nullopt : repl::rpcf::uint_or(&it->second, 0);
        if (!rank || *rank > 2) {
            guild_reply(conn, false, kGuildMemberRankType, "A rank change names no rank this guild has.");
            return;
        }
        GuildMemberRow* row = guild_member_of(*guild_, guid);
        if (!row) {
            guild_reply(conn, false, kGuildMemberRankType, "That player is not in your guild.");
            return;
        }
        if (*rank == 0) {
            guild_reply(conn, false, kGuildMemberRankType, "Only the guild's leader can make another member its leader.");
            return;
        }
        if (guild_rank_ != kGuildLeaderRank && (row->rank <= guild_rank_ || static_cast<int>(*rank) <= guild_rank_)) {
            guild_reply(conn, false, kGuildMemberRankType, "You cannot change the rank of a member of your rank or above, nor to one.");
            return;
        }
        changes.push_back({row, static_cast<int>(*rank)});
    }
    for (const Change& c : changes) c.row->rank = static_cast<uint8_t>(c.rank);
    for (const GuildMemberRow& row : guild_->members) {
        if (PlayerSession* member = guild_session_of(world_, row.guid); member && member->guild_ == guild_) member->guild_rank_ = row.rank;
    }
    guild_save(*guild_, /*create=*/false);
    guild_refresh_members(*guild_, "a member's rank changed");
    guild_reply(conn, true, kGuildMemberRankType, changes.size() == 1 ? "The member's rank changed." : "The members' ranks changed.");
    spdlog::log(level_for(guild_logged_, 20), "[guild] {}: changed {} member rank(s) of {}", name(), changes.size(), guild_->name);
}

void PlayerSession::on_guild_assign_leader(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    if (!guild_) {
        guild_reply(conn, false, kGuildAssignLeaderType, "You are not in a guild.");
        return;
    }
    if (guild_rank_ != kGuildLeaderRank) {
        guild_reply(conn, false, kGuildAssignLeaderType, "Only the guild's leader can name its next leader.");
        return;
    }
    const std::optional<std::array<uint32_t, 4>> guid = guild_call_guid(rpc, "TargetCharacterGuid");
    if (!guid) {
        if (++guild_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[guild] {}: a ServerAssignGuildLeader does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    GuildMemberRow* row = guild_member_of(*guild_, *guid);
    if (!row || row->guid == guild_guid_of(*this)) {
        guild_reply(conn, false, kGuildAssignLeaderType, "That player is not another member of your guild.");
        return;
    }
    const std::string former = name();
    const std::string next = row->name;
    row->rank = 0;
    if (GuildMemberRow* mine = guild_member_of(*guild_, guild_guid_of(*this))) mine->rank = 1;
    for (const GuildMemberRow& member : guild_->members) {
        if (PlayerSession* s = guild_session_of(world_, member.guid); s && s->guild_ == guild_) s->guild_rank_ = member.rank;
    }
    guild_save(*guild_, /*create=*/false);
    guild_refresh_members(*guild_, "the leader changed");
    guild_reply(conn, true, kGuildAssignLeaderType, former + " has reassigned " + next + " as the " + guild_->ranks[0].name);
    spdlog::info("[guild] {}: made {} the leader of {}", name(), guild_shown(next), guild_->name);
}

void PlayerSession::on_guild_get_data(Connection& conn, Clock::time_point now) {
    (void)now;
    if (!guild_) {
        guild_reply(conn, false, kGuildMembersType, "You are not in a guild.");
        return;
    }
    guild_reply(conn, true, kGuildMembersType, "");   // the state itself is the component's; the call is answered
    guild_refresh_pawn(conn);
}

void PlayerSession::on_guild_update_hierarchy(Connection& conn, Clock::time_point now) {
    (void)now;
    if (!guild_ || !guild_may(*guild_, 1ull << 10)) {   // bManageGuildRanks
        guild_reply(conn, false, kGuildRankHierarchyType, guild_ ? "You do not have permission to manage the guild's ranks." : "You are not in a guild.");
        return;
    }
    guild_reply(conn, false, kGuildRankHierarchyType, "This guild's three ranks are the game's own and cannot be changed.");
}

void PlayerSession::on_guild_message(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    if (!guild_) {
        receive_chat_line(conn, "You are not in a guild.", 124, "Server");
        return;
    }
    if (!guild_may(*guild_, kGuildPermWriteChat)) {
        receive_chat_line(conn, "You do not have permission to use guild chat.", 124, "Server");
        return;
    }
    const std::optional<std::string> sent = guild_call_text(rpc, "Message");
    const auto category = rpc.call.decoded ? repl::rpcf::uint_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Category"), 0) : std::nullopt;
    if (!sent || !category) {
        if (++guild_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[guild] {}: a ServerSendMessageToGuild does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    const std::string line = *sent;
    if (line.empty() || line.size() > 512 || std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c) != 0; })) {
        receive_chat_line(conn, "A guild message cannot be empty or over 512 characters.", 124, "Server");
        return;
    }
    size_t reached = 0;
    for (const GuildMemberRow& row : guild_->members) {
        if (row.rank >= guild_->ranks.size() || (guild_->ranks[row.rank].permissions & kGuildPermReadChat) == 0) continue;
        PlayerSession* member = guild_session_of(world_, row.guid);
        if (!member || member->guild_ != guild_) continue;   // (an offline member has no connection: the chat is not saved for it)
        Connection* member_conn = member->chat_entry_.connection;
        if (!member_conn) continue;
        if (member->receive_chat_line(*member_conn, line, static_cast<int>(*category), name())) ++reached;
    }
    spdlog::log(level_for(guild_logged_, 20), "[guild] {}: guild chat reached {} member(s)", name(), reached);
}

void PlayerSession::guild_reply(Connection& conn, bool ok, uint64_t request_type, const std::string& message) {
    queue(ok ? "ClientGuildRequestSucceeded" : "ClientGuildRequestFailed",
          [ok, request_type, message](bootstrap::PlayerBootstrap& pb) { return pb.guild_reply(ok, request_type, message); }, conn, /*must_build=*/true);
}

// ---- the tick and the session's end -----------------------------------------------------------------------------------------

void PlayerSession::run_guild(Connection& conn, Clock::time_point now) {
    // the invitation's 60 s
    if (!guild_pending_name_.empty() && now >= guild_pending_expires_) {
        guild_cancel_pending();
        spdlog::log(level_for(guild_logged_, 20), "[guild] {}: its invitation expired", name());
    }
    if (!guild_) return;
    // the roster's own level follows the member's
    const int level = progress().level;
    if (guild_level_seen_ != level) {
        if (GuildMemberRow* row = guild_member_of(*guild_, guild_guid_of(*this))) {
            row->level = static_cast<uint8_t>(std::clamp(level, 1, 25));
            guild_level_seen_ = level;
            guild_save(*guild_, /*create=*/false);
            guild_refresh_members(*guild_, "a member's level changed");
        }
    }
    (void)conn;
}

void PlayerSession::leave_guild_on_finish() {
    guild_cancel_pending();   // (the guild that invited this session forgets the invitation: nobody can answer it any more)
    if (!guild_) return;
    if (!guild_alive(guild_)) {
        guild_ = nullptr;
        guild_rank_ = -1;
        guild_name_.clear();
        return;
    }
    if (GuildMemberRow* row = guild_member_of(*guild_, guild_guid_of(*this))) {
        row->level = static_cast<uint8_t>(std::clamp(progress().level, 1, 25));
        row->last_online = static_cast<uint64_t>(std::max<int64_t>(0, wall_ms()));
    }
    Guild* g = guild_;
    guild_save(*g, /*create=*/false);
    guild_refresh_members(*g, "a member logged out");
    guild_ = nullptr;
    guild_rank_ = -1;
    guild_name_.clear();
}

void PlayerSession::forget_guild() {
    guild_cancel_pending();
    if (!guild_) return;
    if (!guild_alive(guild_)) {
        guild_ = nullptr;
        guild_rank_ = -1;
        guild_name_.clear();
        return;
    }
    if (GuildMemberRow* row = guild_member_of(*guild_, guild_guid_of(*this))) row->last_online = static_cast<uint64_t>(std::max<int64_t>(0, wall_ms()));
    Guild* g = guild_;
    guild_ = nullptr;
    guild_rank_ = -1;
    guild_name_.clear();
    guild_refresh_members(*g, "a session ended");
}

void PlayerSession::sync_shown_guild() {
    if (guild_name_ == shown_guild_name_) return;
    bool ok = true;
    if (!show_in_pawn(world::Change::State, [&](world::WorldActor& w) { ok = pb_.set_observer_guild(w.actor, guild_name_); })) return;
    if (!ok) {
        if (++shown_failures_ <= kLoggedPerKind) spdlog::warn("[players] {}: the pawn the other players are shown does not take the guild's name", name());
        return;
    }
    shown_guild_name_ = guild_name_;
}

// ---- the GM command ---------------------------------------------------------------------------------------------------------

PlayerSession::GmGuildResult PlayerSession::gm_guild(Connection& conn, const std::string& action, const std::string& guild_name, const std::string& rank, Clock::time_point now,
                                                     const std::string& by) {
    (void)now;
    GmGuildResult r;
    if (action == "leave") {
        if (!guild_) {
            r.refusal = name() + " is in no guild";
            return r;
        }
        Guild* g = guild_;
        const std::string left = g->name;
        guild_remove_member(*g, guild_guid_of(*this), /*tell=*/true, "a GM took a member out");
        guild_cooldown_until_ = wall_ms() + kGuildCooldownMs;
        guild_save_cooldown(guild_guid_of(*this), guild_cooldown_until_);
        pb_.set_guild_alignment({});
        note_shown_alignment();
        if (g->members.empty()) {
            guild_save_disbanded(*g);
            guild_erase(g);
        } else {
            guild_save(*g, /*create=*/false);
        }
        guild_refresh_pawn(conn);
        r.ok = true;
        r.text = name() + " left " + left + " (" + by + ")";
        spdlog::info("[gm] {}: took {} out of the guild {}", by, name(), left);
        return r;
    }
    const std::string rank_name = rank.empty() ? "member" : rank;   // (the command's hint: empty is member)
    const int wanted = rank_name == "leader" ? 0 : rank_name == "officer" ? 1 : rank_name == "member" ? 2 : -1;
    if (action != "join" || wanted < 0) {
        r.refusal = "say action=join with rank=leader, officer or member, or action=leave";
        return r;
    }
    // the guild it joins is found, or made here when none has the name and the rank is leader: all of it is checked before the character leaves the guild it is in
    Guild* g = guild_by_name(guild_name);
    std::unique_ptr<Guild> fresh;
    if (!g) {
        if (wanted != kGuildLeaderRank) {
            r.refusal = "the guild \"" + guild_shown(guild_name) + "\" does not exist; a GM makes a character its leader to create one";
            return r;
        }
        std::string why;
        if (!guild_name_ok(guild_name, why)) {
            r.refusal = why;
            return r;
        }
        fresh = guild_found(guild_name, *this, wall_ms());
        if (!guild_register(*fresh)) {   // (a guild none of whose members is online here has the name)
            r.refusal = "the guild name \"" + guild_shown(guild_name) + "\" is taken";
            return r;
        }
        g = fresh.get();
    } else if (g == guild_) {
        // a rank change inside the guild it is in
        if (guild_rank_ == wanted) {
            r.ok = true;
            r.text = name() + " is already a " + rank_name + " of " + guild_->name;
            return r;
        }
        if (wanted != kGuildLeaderRank && guild_rank_ == kGuildLeaderRank) {
            bool another_leader = false;
            for (const GuildMemberRow& row : g->members) another_leader = another_leader || (row.rank == kGuildLeaderRank && row.guid != guild_guid_of(*this));
            if (!another_leader) {
                r.refusal = "the guild \"" + guild_shown(g->name) + "\" would have no leader: make another character its leader first";
                return r;
            }
        }
    } else if (g->members.size() >= static_cast<size_t>(std::max(1, g->capacity))) {
        r.refusal = "the guild \"" + guild_shown(g->name) + "\" is full";
        return r;
    } else if (g->members.empty()) {
        r.refusal = "the guild \"" + guild_shown(g->name) + "\" has no leader yet: make this character its leader first";
        return r;
    }
    if (guild_ && guild_ != g) {
        Guild* old = guild_;
        guild_remove_member(*old, guild_guid_of(*this), /*tell=*/true, "a GM moved a member");
        if (old->members.empty()) {
            guild_save_disbanded(*old);
            guild_erase(old);
        } else {
            guild_save(*old, /*create=*/false);
        }
    }
    const bool created = fresh != nullptr;
    if (fresh) guild_book().push_back(std::move(fresh));
    if (wanted == kGuildLeaderRank && !g->members.empty()) {
        // a GM's leader joins as the leader: the old leader is demoted to an officer
        for (GuildMemberRow& row : g->members) {
            if (row.rank == kGuildLeaderRank && row.guid != guild_guid_of(*this)) {
                row.rank = 1;
                if (PlayerSession* s = guild_session_of(world_, row.guid); s && s->guild_ == g) s->guild_rank_ = 1;
            }
        }
    }
    guild_add_member(*g, *this, wanted, wall_ms());
    if (!created) guild_save(*g, /*create=*/false);   // (a guild made just now was written whole with its leader by guild_register, or is queued as a create)
    guild_refresh_pawn(conn);
    pb_.set_guild_alignment(g->id);
    note_shown_alignment();
    guild_refresh_members(*g, "a GM added a member");
    r.ok = true;
    r.text = name() + " is a " + rank_name + " of " + g->name + " (" + by + ")";
    spdlog::info("[gm] {}: put {} into the guild {} as {}", by, name(), g->name, rank_name);
    return r;
}

// ---- the calls' registration ------------------------------------------------------------------------------------------------

void PlayerSession::register_guild_rpcs(RpcTable& t) {
    t.add(RpcScope::GuildComponent, "ServerCreateGuild", [](PlayerSession& s, const RpcContext& c) { s.on_guild_create(c.conn, c.rpc, c.now); });
    t.add(RpcScope::GuildComponent, "ServerInvitePlayerToGuild", [](PlayerSession& s, const RpcContext& c) { s.on_guild_invite(c.conn, c.rpc, c.now); });
    t.add(RpcScope::GuildComponent, "ServerAcceptInviteToGuild", [](PlayerSession& s, const RpcContext& c) { s.on_guild_accept(c.conn, c.now); });
    t.add(RpcScope::GuildComponent, "ServerDeclineInviteToGuild", [](PlayerSession& s, const RpcContext& c) { s.on_guild_decline(c.conn, c.now); });
    t.add(RpcScope::GuildComponent, "ServerLeaveGuild", [](PlayerSession& s, const RpcContext& c) { s.on_guild_leave(c.conn, c.now); });
    t.add(RpcScope::GuildComponent, "ServerKickPlayerFromGuild", [](PlayerSession& s, const RpcContext& c) { s.on_guild_kick(c.conn, c.rpc, c.now); });
    t.add(RpcScope::GuildComponent, "ServerDisbandGuild", [](PlayerSession& s, const RpcContext& c) { s.on_guild_disband(c.conn, c.now); });
    t.add(RpcScope::GuildComponent, "ServerReassignMemberRanks", [](PlayerSession& s, const RpcContext& c) { s.on_guild_reassign(c.conn, c.rpc, c.now); });
    t.add(RpcScope::GuildComponent, "ServerAssignGuildLeader", [](PlayerSession& s, const RpcContext& c) { s.on_guild_assign_leader(c.conn, c.rpc, c.now); });
    t.add(RpcScope::GuildComponent, "ServerGetUpdatedGuildData", [](PlayerSession& s, const RpcContext& c) { s.on_guild_get_data(c.conn, c.now); });
    t.add(RpcScope::GuildComponent, "ServerUpdateRankHierarchy", [](PlayerSession& s, const RpcContext& c) { s.on_guild_update_hierarchy(c.conn, c.now); });
    t.add(RpcScope::GuildComponent, "ServerSendMessageToGuild", [](PlayerSession& s, const RpcContext& c) { s.on_guild_message(c.conn, c.rpc, c.now); });
}

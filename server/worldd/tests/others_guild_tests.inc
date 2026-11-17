// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/others_guild_tests.inc: a guildmate's name and colour as another player's client gets them (Social, step 9)
//
//  A guilded player (cyb) and a watcher (ayla) in one world (tests/mounts_ride_others_rig.h).  What the watcher's client is sent for the player's pawn: the pawn's
//  guild component carries the guild's name (the nameplate's line), and the AlignmentComponent carries the GuildMember instance with the guild's GUID (the colour
//  the client gives a guildmate).  Both stay when the flag's own alignment is there (the PvP colours and the guild's meet in the same container), and a watcher
//  who comes into view later gets them in the open.  When the player leaves its guild, the name and the instance go.
//
//  Not tested: no game client here, so the colour the client picks among two overriding alignments (the guild's and a flag's) is not seen; the container's
//  order is the game's (the guild's instance after the four the join sets).
// ============================================================================

/// The guild the two tests below use.
static constexpr uint64_t kOthersGuildAlignment = 6064630560782155776ull;

/// A guild id (32 hex digits) for these tests: the four words the id "12ab34cd56ef789012ab34cd56ef7890" parses to.
static std::array<uint32_t, 4> others_guild_id() { return {0x12ab34cd, 0x56ef7890, 0x12ab34cd, 0x56ef7890}; }

/// cyb's record in a guild of "Sandal Makers", its leader.
static svc::Character guilded_cyb() {
    svc::Character c = cyb_record();
    svc::Guild g;
    g.id = "12ab34cd56ef789012ab34cd56ef7890";
    g.name = "Sandal Makers";
    g.version = 2;
    g.ranks = {{"Guild Leader", (1ull << 35) - 1}, {"Officer", 0x7ffcfc7f}, {"Member", 0x65006014}};
    g.level = 1;
    g.capacity = 50;
    g.level_cap = 10;
    svc::GuildMember row;
    row.character_id = c.id;
    row.name = c.name;
    row.rank = 0;
    row.level = c.level;
    row.class_id = c.class_id;
    row.joined_ms = 1763000000000;
    g.members.push_back(row);
    g.self_rank = 0;
    c.guild = g;
    return c;
}

/// The guild component's block in a chain of the watcher, if the chain carries one (the pawn's open and its updates).
static const repl::ObjectState* guild_state_in(const States& st, const PlayerSession& of) {
    const auto it = st.find(of.bootstrap().setup().guids.guild.ObjectId);
    return it == st.end() ? nullptr : &it->second;
}

/// The state the watcher's chains gave the object `object` last (the chains are read in order; nullopt: never).
static std::optional<repl::ObjectState> last_state_of(const Harness& watcher, const repl::Actor& actor, uint64_t object) {
    std::optional<repl::ObjectState> found;
    for (const fake::Delivered& d : watcher.got) {
        const States st = chain_states(d, actor);
        const auto it = st.find(object);
        if (it != st.end()) found = it->second;
    }
    return found;
}

/// The guild's name the watcher's chains carry for the player's pawn (empty when none): the last GuildName a guild block of the pawn carries.
static std::string watched_guild_name(const Harness& watcher, const world::WorldActor& pawn, const PlayerSession& of) {
    std::string found;
    for (const fake::Delivered& d : watcher.got) {
        const States st = chain_states(d, pawn.actor);
        const auto it = st.find(of.bootstrap().setup().guids.guild.ObjectId);
        if (it == st.end() || !it->second.schema) continue;
        const repl::Value* v = it->second.state.get(*it->second.schema, "GuildName");
        const std::string* text = v ? v->get_if<std::string>() : nullptr;
        if (text) found = *text;
    }
    return found;
}

/// The AlignmentContainer the watcher's chains carry for the player's pawn (nullopt: none).
static std::optional<repl::AlignmentContainer> watched_alignments(const Harness& watcher, const world::WorldActor& pawn, const PlayerSession& of) {
    const auto st = last_state_of(watcher, pawn.actor, of.bootstrap().setup().guids.alignment.ObjectId);
    if (!st || !st->schema) return std::nullopt;
    const repl::Value* v = st->state.get(*st->schema, "Alignments");
    const auto* c = v ? v->get_if<repl::AlignmentContainer>() : nullptr;
    if (!c) return std::nullopt;
    return *c;
}

/// True when `c` holds an instance with this alignment key and GUID.
static bool has_guild_instance(const repl::AlignmentContainer& c, const std::array<uint32_t, 4>& guild) {
    for (const repl::AlignmentContainer::Instance& i : c.instances) {
        if (i.key != kOthersGuildAlignment) continue;
        for (const repl::GuidValue& g : i.guids) {
            if (g.a == guild[0] && g.b == guild[1] && g.c == guild[2] && g.d == guild[3]) return true;
        }
    }
    return false;
}

/// True when any chain the watcher was given carried the GuildMember instance of `guild` in the pawn's AlignmentComponent (the client keeps it from then on: a
/// container's block is the whole container, and a later block without it would have removed it; the server sets the whole container every time).
static bool watched_has_guild_instance(const Harness& watcher, const world::WorldActor& pawn, const PlayerSession& of, const std::array<uint32_t, 4>& guild) {
    for (const fake::Delivered& d : watcher.got) {
        const States st = chain_states(d, pawn.actor);
        const auto it = st.find(of.bootstrap().setup().guids.alignment.ObjectId);
        if (it == st.end() || !it->second.schema) continue;
        const repl::Value* v = it->second.state.get(*it->second.schema, "Alignments");
        const auto* c = v ? v->get_if<repl::AlignmentContainer>() : nullptr;
        if (c && has_guild_instance(*c, guild)) return true;
    }
    return false;
}

/// True when the *last* alignment container the watcher was given carries the instance (a removal is seen only in the last one).
static bool watched_last_has_guild_instance(const Harness& watcher, const world::WorldActor& pawn, const PlayerSession& of, const std::array<uint32_t, 4>& guild) {
    const auto st = last_state_of(watcher, pawn.actor, of.bootstrap().setup().guids.alignment.ObjectId);
    if (!st || !st->schema) return false;
    const repl::Value* v = st->state.get(*st->schema, "Alignments");
    const auto* c = v ? v->get_if<repl::AlignmentContainer>() : nullptr;
    return c && has_guild_instance(*c, guild);
}

/// True when the player's own pawn's AlignmentComponent carries the instance.
static bool own_has_guild_instance(const PlayerSession& s, const std::array<uint32_t, 4>& guild) {
    for (const repl::Subobject& sub : s.bootstrap().pawn().subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAlignmentComponent") continue;
        const repl::Value* v = sub.obj.state.get(*sub.obj.schema, "Alignments");
        const auto* c = v ? v->get_if<repl::AlignmentContainer>() : nullptr;
        if (c && has_guild_instance(*c, guild)) return true;
    }
    return false;
}

/// A guild call on the player's guild component, from its player's client.
static bool others_guild_send(Harness& h, const char* name, const repl::RpcParams& params = {}) {
    const schema::ClassSchema& cls = *gen::find_component_schema("GuildComponent");
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return false;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) return false;
    BunchWriter out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.object = h.s->bootstrap().setup().guids.guild;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    h.client_sends({h.client.actor(h.pawn_ch, out, true)});
    return true;
}

static void test_a_watcher_sees_the_guilds_name_and_the_guildmates_colour() {
    std::printf("a watcher's client is sent the guilded player's guild name and the GuildMember instance (the guildmate colour)\n");
    Players players;
    auto a = std::make_unique<Harness>(guilded_cyb());
    a->play();
    auto watcher = join_world(*a, players, other_record(other_id(1), "ayla", 2));
    a->settle();
    watcher->settle();
    const world::WorldActor* pawn = shown_pawn(*a);
    CHECK(pawn != nullptr, "the guilded player's pawn is in the world for the others");
    CHECK(watched_guild_name(*watcher, *pawn, *a->s) == "Sandal Makers", "the watcher's copy of the pawn carries the guild's name (the nameplate)");
    const auto alignments = watched_alignments(*watcher, *pawn, *a->s);
    CHECK(watched_has_guild_instance(*watcher, *pawn, *a->s, others_guild_id()), "and the AlignmentComponent carries the GuildMember instance with the guild's GUID");
    CHECK(alignments && !has_guild_instance(*alignments, {0, 0, 0, 0}), "(never a zero guild)");
    // the flag's own alignment is in the same container: both meet there
    CHECK(alignments && alignments->instances.size() >= 4, "the container keeps the game's own instances beside the guild's");
}

static void test_a_third_player_who_comes_into_view_sees_the_guild() {
    std::printf("a player who comes into view later gets the guild's name and instance in the pawn's open\n");
    Players players;
    auto a = std::make_unique<Harness>(guilded_cyb());
    a->play();
    a->settle();
    auto second = join_world(*a, players, other_record(other_id(2), "dee", 3));
    a->settle();
    second->settle();
    auto late = join_world(*a, players, other_record(other_id(3), "bram", 4));
    a->settle();
    late->settle();
    const world::WorldActor* pawn = shown_pawn(*a);
    CHECK(pawn != nullptr, "the guilded player's pawn is in the world");
    CHECK(watched_guild_name(*late, *pawn, *a->s) == "Sandal Makers", "the late watcher's open carries the guild's name");
    CHECK(watched_has_guild_instance(*late, *pawn, *a->s, others_guild_id()), "...and its open carries the GuildMember instance");
}

static void test_leaving_the_guild_takes_the_name_and_the_colour_off() {
    std::printf("a member who leaves its guild has no guild name and no GuildMember instance on the pawn the others are shown\n");
    Players players;
    auto a = std::make_unique<Harness>(guilded_cyb());
    a->play();
    auto watcher = join_world(*a, players, other_record(other_id(4), "ayla", 5));
    a->settle();
    watcher->settle();
    const world::WorldActor* pawn = shown_pawn(*a);
    CHECK(pawn != nullptr && watched_guild_name(*watcher, *pawn, *a->s) == "Sandal Makers", "(the watcher has the guild's name first)");
    CHECK(others_guild_send(*a, "ServerLeaveGuild"), "the player leaves its guild");
    a->settle();
    watcher->settle();
    pawn = shown_pawn(*a);
    CHECK(pawn != nullptr, "the pawn is still there");
    CHECK(watched_guild_name(*watcher, *pawn, *a->s).empty(), "the watcher's copy carries no guild name any more");
    CHECK(!own_has_guild_instance(*a->s, others_guild_id()), "(the player's own pawn lost the instance)");
    CHECK(!watched_last_has_guild_instance(*watcher, *pawn, *a->s, others_guild_id()), "and no GuildMember instance");
}

AOC_REGISTER_TEST(test_a_watcher_sees_the_guilds_name_and_the_guildmates_colour);
AOC_REGISTER_TEST(test_a_third_player_who_comes_into_view_sees_the_guild);
AOC_REGISTER_TEST(test_leaving_the_guild_takes_the_name_and_the_colour_off);

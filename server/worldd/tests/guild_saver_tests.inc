// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/guild_saver_tests.inc
//
//  Social, step 9: the saver keeps the order of one guild's saves.  The world counts its guild's version as it hands the saves over (a create is
//  version 0, every save writes over the version before it and makes the next one more), and the character service takes a save only over the version it holds.
//  A save that still waits when a newer one of the same guild comes in (the saver keeps the latest state of a guild) must therefore be written over the version
//  the waiting one was to write over: the service never saw the waiting one, so the newer one's own version would be refused as stale, for good.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================
#include "friend_fixes_rig.inc"

namespace {

/// A character service that keeps guilds the way the real one does (version 0 creates, any other version must be the one it holds, and the save makes it one
/// more; a disband needs the version too), on top of the friends tests' fake that can hold the worker.
struct GuildStoreApi : FriendStoreApi {
    std::map<std::string, svc::Guild> guilds;   // id -> as stored (its version is the store's)
    std::vector<std::string>          refusals;
    std::vector<int64_t>              versions_seen;   // the version of every save the service was asked for, in order
    int                               fail_next_with_unavailable = 0;
    bool                              hold_next_guild_save = false;   // the first guild save is held in the worker (with the gate) until released

    svc::CallStatus save_guild(const svc::Guild& g) override {
        if (hold_next_guild_save && gate) {
            hold_next_guild_save = false;
            gate->block();
        }
        std::lock_guard<std::mutex> lock(mu);
        versions_seen.push_back(g.version);
        if (fail_next_with_unavailable > 0) {
            --fail_next_with_unavailable;
            return svc::CallStatus::Unavailable;
        }
        if (g.disband) {
            const auto it = guilds.find(g.id);
            if (it == guilds.end() || it->second.version != g.version) {
                refusals.push_back("a disband over version " + std::to_string(g.version));
                return svc::CallStatus::Failed;
            }
            guilds.erase(it);
            return svc::CallStatus::Ok;
        }
        const auto it = guilds.find(g.id);
        if (g.version == 0) {
            if (it != guilds.end()) {
                refusals.push_back("a create of a guild that exists");
                return svc::CallStatus::Refused;
            }
            svc::Guild kept = g;
            kept.version = 1;
            guilds[g.id] = kept;
            return svc::CallStatus::Ok;
        }
        if (it == guilds.end()) {
            refusals.push_back("a save over version " + std::to_string(g.version) + " of a guild the service does not hold");
            return svc::CallStatus::NotFound;
        }
        if (it->second.version != g.version) {
            refusals.push_back("a save over version " + std::to_string(g.version) + ", the service holds " + std::to_string(it->second.version));
            return svc::CallStatus::Failed;
        }
        svc::Guild kept = g;
        kept.version = g.version + 1;
        it->second = kept;
        return svc::CallStatus::Ok;
    }
};

/// A guild of `members` members named after the save's number, at the version the world counts.
svc::Guild guild_save_of(const std::string& id, int64_t version, size_t members, bool disband = false) {
    svc::Guild g;
    g.id = id;
    g.name = "Saver Guild";
    g.version = version;
    g.ranks = {{"Guild Leader", (1ull << 35) - 1}, {"Officer", 0x7ffcfc7full}, {"Member", 0x65006014ull}};
    for (size_t i = 0; i < members; ++i) {
        svc::GuildMember m;
        char hex[33];
        std::snprintf(hex, sizeof hex, "%032llx", 0x2222000000000000ull + i + 1);
        m.character_id = hex;
        m.name = "member" + std::to_string(i);
        m.rank = i == 0 ? 0 : 2;
        g.members.push_back(m);
    }
    g.disband = disband;
    return g;
}

const char* const kSaverGuildId = "22220000000000000000000000000001";
const char* const kHoldId = "ffffffffffffffffffffffffffffffff";

}  // namespace

static void test_saves_of_one_guild_that_wait_are_written_over_the_first_ones_version() {
    std::printf("a guild's saves that wait together are written as one, over the version the first of them was to write over\n");
    GuildStoreApi api;
    FriendStoreApi::Gate gate;
    api.gate = &gate;
    api.gate_id = kHoldId;
    api.gate_progress = false;
    CharacterSaver::Config config;
    config.max_attempts = 1;
    config.destructor_wait = std::chrono::milliseconds(500);
    CharacterSaver saver(api, config);
    saver.save(kHoldId, svc::Position{});   // (the worker is held on another character's position while the guild's saves queue)
    if (!gate.wait_entered()) {
        CHECK(false, "(the worker is held)");
        gate.release();
        return;
    }
    // the world made the guild and changed it twice before the service saw any of it: a create, then two saves over versions 1 and 2
    saver.save_guild(guild_save_of(kSaverGuildId, 0, 1));
    saver.save_guild(guild_save_of(kSaverGuildId, 1, 2));
    saver.save_guild(guild_save_of(kSaverGuildId, 2, 3));
    gate.release();
    CHECK(saver.flush(std::chrono::seconds(5)), "(the saver wrote the guild)");
    std::lock_guard<std::mutex> lock(api.mu);
    CHECK(api.refusals.empty(), ("the service refused no save" + (api.refusals.empty() ? std::string() : ": " + api.refusals.front())).c_str());
    CHECK(api.guilds.size() == 1 && api.guilds.begin()->second.members.size() == 3, "the guild is stored with its latest state (three members)");
    CHECK(api.versions_seen == std::vector<int64_t>{0}, "...written once, as the create the first of the three was");
}

static void test_updates_that_wait_together_are_written_over_the_version_the_service_holds() {
    std::printf("updates of a stored guild that wait together are written over the version the service holds\n");
    GuildStoreApi api;
    svc::Guild stored = guild_save_of(kSaverGuildId, 7, 2);
    api.guilds[kSaverGuildId] = stored;
    FriendStoreApi::Gate gate;
    api.gate = &gate;
    api.gate_id = kHoldId;
    api.gate_progress = false;
    CharacterSaver::Config config;
    config.max_attempts = 1;
    config.destructor_wait = std::chrono::milliseconds(500);
    CharacterSaver saver(api, config);
    saver.save(kHoldId, svc::Position{});
    if (!gate.wait_entered()) {
        CHECK(false, "(the worker is held)");
        gate.release();
        return;
    }
    saver.save_guild(guild_save_of(kSaverGuildId, 7, 3));
    saver.save_guild(guild_save_of(kSaverGuildId, 8, 4));
    saver.save_guild(guild_save_of(kSaverGuildId, 9, 5));
    gate.release();
    CHECK(saver.flush(std::chrono::seconds(5)), "(the saver wrote the guild)");
    std::lock_guard<std::mutex> lock(api.mu);
    CHECK(api.refusals.empty(), ("the service refused no save" + (api.refusals.empty() ? std::string() : ": " + api.refusals.front())).c_str());
    CHECK(api.guilds[kSaverGuildId].members.size() == 5 && api.guilds[kSaverGuildId].version == 8, "the stored guild has the latest state, one version further");
}

static void test_a_guild_made_and_done_away_with_before_the_service_saw_it_writes_nothing() {
    std::printf("a guild made and disbanded while its create still waits is never written\n");
    GuildStoreApi api;
    FriendStoreApi::Gate gate;
    api.gate = &gate;
    api.gate_id = kHoldId;
    api.gate_progress = false;
    CharacterSaver::Config config;
    config.max_attempts = 1;
    config.destructor_wait = std::chrono::milliseconds(500);
    CharacterSaver saver(api, config);
    saver.save(kHoldId, svc::Position{});
    if (!gate.wait_entered()) {
        CHECK(false, "(the worker is held)");
        gate.release();
        return;
    }
    saver.save_guild(guild_save_of(kSaverGuildId, 0, 1));
    saver.save_guild(guild_save_of(kSaverGuildId, 1, 1, /*disband=*/true));
    gate.release();
    CHECK(saver.flush(std::chrono::seconds(5)), "(the saver finished)");
    std::lock_guard<std::mutex> lock(api.mu);
    CHECK(api.versions_seen.empty() && api.guilds.empty() && api.refusals.empty(), "the service was asked nothing and holds no guild");
}

static void test_a_save_that_is_tried_again_and_a_newer_one_are_written_over_the_same_version() {
    std::printf("a guild save the service could not take yet and a newer one of the guild come out as one write over the version of the first\n");
    GuildStoreApi api;
    api.guilds[kSaverGuildId] = guild_save_of(kSaverGuildId, 4, 2);
    FriendStoreApi::Gate gate;
    api.gate = &gate;
    api.gate_id = kHoldId;
    api.hold_next_guild_save = true;
    api.fail_next_with_unavailable = 1;
    CharacterSaver::Config config;
    config.max_attempts = 3;
    config.retry_delay = std::chrono::milliseconds(20);
    config.destructor_wait = std::chrono::milliseconds(500);
    CharacterSaver saver(api, config);
    saver.save_guild(guild_save_of(kSaverGuildId, 4, 3));   // (held in the worker, then answered Unavailable)
    if (!gate.wait_entered()) {
        CHECK(false, "(the worker is in the guild's save)");
        gate.release();
        return;
    }
    saver.save_guild(guild_save_of(kSaverGuildId, 5, 4));   // (a newer save of the guild, while the first is in the worker)
    gate.release();
    CHECK(saver.flush(std::chrono::seconds(5)), "(the saver finished)");
    std::lock_guard<std::mutex> lock(api.mu);
    CHECK(api.refusals.empty(), ("the service refused no save" + (api.refusals.empty() ? std::string() : ": " + api.refusals.front())).c_str());
    CHECK(api.guilds[kSaverGuildId].members.size() == 4 && api.guilds[kSaverGuildId].version == 5, "the service holds the newer state, one version further than the first save wrote over");
}

AOC_REGISTER_TEST(test_saves_of_one_guild_that_wait_are_written_over_the_first_ones_version);
AOC_REGISTER_TEST(test_updates_that_wait_together_are_written_over_the_version_the_service_holds);
AOC_REGISTER_TEST(test_a_guild_made_and_done_away_with_before_the_service_saw_it_writes_nothing);
AOC_REGISTER_TEST(test_a_save_that_is_tried_again_and_a_newer_one_are_written_over_the_same_version);

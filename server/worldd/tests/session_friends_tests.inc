// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_friends_tests.inc
//
//  Registered tests of test_worldd_session: social step 7, the friends list's store.  This round keeps the lists only: the character service stores a character's friends and pending
//  friend requests (migration 0024), worldd reads them with the character at the join and saves them whole with the progress.  No call and no component is built here (the calls, the
//  export and the updates are a later round), so the tests check what the session holds, what it hands to the saver, what a character with 2 friends and 1 pending request has after a
//  logout and a new login, what a login elsewhere takes from the old session or from the saver, and what the join cuts (an expired request, a list over its cap).
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================
#include <condition_variable>
#include <mutex>

namespace {

/// The expiry of the live request in the tests: a time far in the future (2099), so the wall clock never makes it stale.
constexpr int64_t kLiveExpiryMs = 4102444800000ll;

/// Where the session hands what it saves: the progress hand-overs, with the character they were for.
struct FriendSink : PositionSink {
    std::vector<std::pair<std::string, svc::Progress>> progress;
    void save(const std::string&, const svc::Position&) override {}
    void save_progress(const std::string& id, svc::Progress p) override { progress.emplace_back(id, std::move(p)); }
};

/// cyb's record with 2 friends and 1 pending request, in the order the character service sends them.
static svc::Character friends_record() {
    svc::Character c = cyb_record();
    c.friends = {{"0123456789abcdef0123456789abcdef", "Brannoc", 25, 7, 2, "Sandal Makers", true},
                 {"fedcba9876543210fedcba9876543210", "Mirelle", 1, 1, 0, "", false}};
    c.friend_requests = {{"11111111111111111111111111111111", "Brannoc", kCyb, kLiveExpiryMs}};
    return c;
}

/// A friend guid for the nth generated friend (32 lower-case hex digits, distinct from the two above).
static std::string extra_friend_guid(int n) {
    char text[33];
    std::snprintf(text, sizeof text, "%032x", n);
    return text;
}

/// Joins `rec` with `sink` as the saver and enters the world (the phase-in sent and everything settled).
static bool friends_enter_world(Harness& h, FriendSink& sink, const svc::Character& rec) {
    h.fx.world->set_position_sink(&sink);
    if (!h.join(rec) || !h.s || h.s->failed()) return false;
    h.settle();
    h.client_sends({h.client.actor(h.s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    return true;
}

/// The newest hand-over of the progress, or nullptr.
static const svc::Progress* friend_last_progress(const FriendSink& sink) { return sink.progress.empty() ? nullptr : &sink.progress.back().second; }

}  // namespace

static void test_the_friends_and_requests_are_loaded_at_the_join_and_saved_with_the_progress() {
    std::printf("the character's friends and pending requests are held at the join and go whole with the progress\n");
    Harness h;
    FriendSink sink;
    svc::Character rec = friends_record();
    rec.completed_ftues = {123};   // (a finished tutorial whose putting back hands the progress over)
    if (!friends_enter_world(h, sink, rec)) {
        CHECK(false, "the session joins and enters the world");
        return;
    }
    CHECK(h.s->friends() == rec.friends, "the session holds the 2 friends, in the saved order and with every field");
    CHECK(h.s->friend_requests() == rec.friend_requests, "and the pending request");
    CHECK(sink.progress.empty(), "loading the lists saves nothing");
    const size_t removed = h.s->gm_reset_tutorials(h.conn, "the friend test");
    CHECK(removed == 1, "(a finished tutorial was put back, which hands the progress over)");
    const svc::Progress* saved = friend_last_progress(sink);
    CHECK(saved && saved->replace_friends && saved->friends == rec.friends && saved->replace_friend_requests && saved->friend_requests == rec.friend_requests,
          "the progress carries both lists whole, in order, with the replace flags");
    h.s->finish("the friend test");
    const svc::Progress* last = friend_last_progress(sink);
    CHECK(sink.progress.size() == 2 && last && last->replace_friends && last->friends == rec.friends && last->replace_friend_requests &&
              last->friend_requests == rec.friend_requests,
          "the end of the session hands them over once more");
}

AOC_REGISTER_TEST(test_the_friends_and_requests_are_loaded_at_the_join_and_saved_with_the_progress);

static void test_a_character_with_friends_logs_out_and_in_again_with_the_same_lists() {
    std::printf("a character with 2 friends and 1 pending request logs out and joins again with the same lists\n");
    const svc::Character rec = friends_record();
    {
        Harness h;
        FriendSink sink;
        if (!friends_enter_world(h, sink, rec)) {
            CHECK(false, "the first session joins");
            return;
        }
        CHECK(h.s->friends() == rec.friends && h.s->friend_requests() == rec.friend_requests, "the first session holds the 2 friends and the request");
        h.s->finish("logged out");
    }
    {
        Harness h;
        FriendSink sink;
        if (!friends_enter_world(h, sink, rec)) {
            CHECK(false, "the session after the logout joins");
            return;
        }
        CHECK(h.s->friends() == rec.friends, "the next session holds the same 2 friends, in order");
        CHECK(h.s->friend_requests() == rec.friend_requests, "and the same pending request");
    }
}

AOC_REGISTER_TEST(test_a_character_with_friends_logs_out_and_in_again_with_the_same_lists);

static void test_a_login_elsewhere_keeps_the_friends_and_requests() {
    std::printf("a login elsewhere takes the friends and the pending requests from the session it replaces\n");
    Fixture fx;
    FriendSink sink;
    fx.world->set_position_sink(&sink);
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    const svc::Character rec = friends_record();
    Joined a = handshake(rig, kA, t0);
    log_in(rig, a, kA, rec, t0);
    join_now(rig, a, kA, t0 + milliseconds(40));
    const PlayerSession* pa = rig.ep.player_at(kA);
    CHECK(pa && pa->friends() == rec.friends && pa->friend_requests() == rec.friend_requests, "(the first session holds the lists)");
    // the second login read the character before the lists were saved: the record it read has none
    Joined b = handshake(rig, kB, t0 + milliseconds(200));
    log_in(rig, b, kB, cyb_record(), t0 + milliseconds(210));
    join_now(rig, b, kB, t0 + milliseconds(250));
    const PlayerSession* pb = rig.ep.player_at(kB);
    CHECK(pb && pb->friends() == rec.friends, "the second session holds the first one's 2 friends");
    CHECK(pb && pb->friend_requests() == rec.friend_requests, "and its pending request");
}

AOC_REGISTER_TEST(test_a_login_elsewhere_keeps_the_friends_and_requests);

static void test_an_expired_request_and_a_list_over_the_cap_are_cut_at_the_join() {
    std::printf("the join leaves out an expired request and keeps the friends to their cap of 100\n");
    Harness h;
    FriendSink sink;
    svc::Character rec = friends_record();
    for (int i = 1; i <= 101; ++i) rec.friends.push_back({extra_friend_guid(i), "Extra" + std::to_string(i), 1, 1, 0, "", true});
    rec.friend_requests.push_back({"22222222222222222222222222222222", "Tavish", kCyb, 1000000000000ll});   // (2001: stale)
    if (!friends_enter_world(h, sink, rec)) {
        CHECK(false, "the session joins");
        return;
    }
    CHECK(h.s->friends().size() == bootstrap::kMaxFriends, "the friends stop at the cap of 100");
    const std::vector<svc::Friend>& kept = h.s->friends();
    CHECK(!kept.empty() && kept.front() == rec.friends.front() && kept.back().name == "Extra98", "the first 100 are kept, in order");
    CHECK(h.s->friend_requests().size() == 1 && h.s->friend_requests()[0].sender_name == "Brannoc", "the expired request is left out and the live one stays");
}

AOC_REGISTER_TEST(test_an_expired_request_and_a_list_over_the_cap_are_cut_at_the_join);

static void test_a_character_without_lists_saves_them_empty() {
    std::printf("a character without lists still saves both lists as empty replacements\n");
    Harness h;
    FriendSink sink;
    svc::Character rec = cyb_record();
    rec.completed_ftues = {123};
    if (!friends_enter_world(h, sink, rec)) {
        CHECK(false, "the session joins");
        return;
    }
    CHECK(h.s->friends().empty() && h.s->friend_requests().empty(), "the session holds no list");
    h.s->gm_reset_tutorials(h.conn, "the friend test");
    const svc::Progress* saved = friend_last_progress(sink);
    CHECK(saved && saved->replace_friends && saved->friends.empty() && saved->replace_friend_requests && saved->friend_requests.empty(),
          "the progress says both lists are empty, so a list the character had is cleared");
}

AOC_REGISTER_TEST(test_a_character_without_lists_saves_them_empty);

/// A character service that keeps what the saver writes; `gate` holds the worker in a position call for `block_for`, so a later progress stays queued in the saver.
struct FriendSaveApi : svc::CharacterApi {
    struct Gate {
        std::mutex              mu;
        std::condition_variable cv;
        bool                    entered = false, open = false;

        bool wait_until_entered() {
            std::unique_lock<std::mutex> lock(mu);
            return cv.wait_for(lock, std::chrono::seconds(3), [this] { return entered; });
        }
        void block() {
            std::unique_lock<std::mutex> lock(mu);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [this] { return open; });
        }
        void release() {
            std::lock_guard<std::mutex> lock(mu);
            open = true;
            cv.notify_all();
        }
    };

    Gate*         gate = nullptr;
    std::string   block_for;
    svc::Progress progress;
    int           saves = 0;
    svc::CharacterResult get_character(const std::string&) override { return {}; }
    svc::CallStatus      save_position(const std::string& id, const svc::Position&) override {
        if (gate && id == block_for) gate->block();
        return svc::CallStatus::Ok;
    }
    svc::CallStatus save_progress(const std::string&, const svc::Progress& p) override {
        progress = p;
        ++saves;
        return svc::CallStatus::Ok;
    }
};

static void test_the_saver_holds_the_newest_lists_for_a_login_elsewhere() {
    std::printf("the saver hands the friends and requests of a progress it still holds to a login elsewhere, and forgets them once written\n");
    FriendSaveApi api;
    FriendSaveApi::Gate gate;
    api.gate = &gate;
    api.block_for = "ffffffffffffffffffffffffffffffff";
    CharacterSaver saver(api);
    saver.save(api.block_for, svc::Position{});   // (the worker is held there, so the progress below stays queued)
    if (!gate.wait_until_entered()) {
        CHECK(false, "(the saver is busy with another character)");
        gate.release();
        return;
    }
    const svc::Character rec = friends_record();
    svc::Progress p;
    p.replace_friends = true;
    p.friends = rec.friends;
    p.replace_friend_requests = true;
    p.friend_requests = rec.friend_requests;
    saver.save_progress(kCyb, p);
    std::vector<svc::Friend> friends;
    std::vector<svc::FriendRequest> requests;
    CHECK(saver.pending_friends(kCyb, friends, requests) && friends == rec.friends && requests == rec.friend_requests,
          "the lists of the progress it still holds are handed over");
    gate.release();
    CHECK(saver.flush(std::chrono::seconds(5)) && api.saves == 1, "(the saver writes the progress)");
    CHECK(!saver.pending_friends(kCyb, friends, requests), "once written the saver holds nothing");
}

AOC_REGISTER_TEST(test_the_saver_holds_the_newest_lists_for_a_login_elsewhere);

/// The saver of a world that keeps the lists of a progress hand-over it has not written yet: a test's sink.
struct PendingFriendSink : PositionSink {
    std::vector<svc::Friend>        friends;
    std::vector<svc::FriendRequest> requests;
    bool                            active = false;   // the old session's progress has reached the saver
    void save(const std::string&, const svc::Position&) override {}
    void save_progress(const std::string&, svc::Progress) override { active = true; }
    bool pending_friends(const std::string& character_id, std::vector<svc::Friend>& out_friends, std::vector<svc::FriendRequest>& out_requests) override {
        (void)character_id;
        if (!active) return false;
        out_friends = friends;
        out_requests = requests;
        return true;
    }
};

static void test_a_login_after_the_old_session_is_gone_takes_the_savers_lists() {
    std::printf("a login that lands after the old session was removed takes the lists the saver still holds\n");
    Fixture fx;
    PendingFriendSink sink;
    const svc::Character rec = friends_record();
    sink.friends = rec.friends;
    sink.requests = rec.friend_requests;
    fx.world->set_position_sink(&sink);
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    log_in(rig, a, kA, cyb_record(), t0);
    join_now(rig, a, kA, t0 + milliseconds(40));
    CHECK(rig.ep.player_at(kA) && rig.ep.player_at(kA)->friends().empty(), "(the record the login read has no lists)");
    // the old session ends and its progress reaches the saver, which still holds the lists (the writer is slower than the next login)
    sink.active = true;
    exchange(rig, a, kA, {a.client->close_channel()}, t0 + milliseconds(120));
    CHECK(!rig.ep.session_of(kCyb), "(the first session has ended)");
    // the same character logs in at another address
    Joined b = handshake(rig, kB, t0 + milliseconds(200));
    log_in(rig, b, kB, cyb_record(), t0 + milliseconds(210));
    join_now(rig, b, kB, t0 + milliseconds(250));
    const PlayerSession* pb = rig.ep.player_at(kB);
    CHECK(pb && pb->friends() == rec.friends, "the new session holds the 2 friends the saver still held");
    CHECK(pb && pb->friend_requests() == rec.friend_requests, "and the pending request");
}

AOC_REGISTER_TEST(test_a_login_after_the_old_session_is_gone_takes_the_savers_lists);

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_login_window_tests.inc
//
//  Finding 2 of the friends-list review: a request that lands while the target is connecting, and one saved after the target's login read.  The
//  request must be on the session's lists and in its next save, whatever moment of the login it arrives in.  The later review's finding 1: that
//  save must not put the joining session's whole login-era progress over the level and XP the session it replaces saved; the two lists go alone.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================
#include "friend_fixes_rig.inc"

static void test_a_request_while_the_target_is_connecting_is_shown_at_the_phase_in() {
    std::printf("a request that arrives while the target is connecting is on its lists and its first save keeps it\n");
    FriendWorld w;
    svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    b.completed_ftues = {123};   // (a finished tutorial whose putting back hands the progress over)
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard"))) {
        CHECK(false, "the sender joins");
        return;
    }
    // the target has joined (its session exists) but has not phased in yet
    if (!w.join(1, b) || !w.p[1].s || w.p[1].s->failed()) {
        CHECK(false, "the target's session is built and has not phased in");
        return;
    }
    w.settle();
    const size_t b_before = w.p[1].got.size();
    w.request(0, friend_send_call("Brannoc"));
    CHECK(w.p[1].s->friend_requests().size() == 1 && w.p[1].s->friend_requests()[0].sender_guid == kFriendA,
          "the connecting target holds the request, not only the character service");
    // the target phases in: the request must be in what the client is shown
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    w.client_sends(w.p[1], {w.p[1].client.actor(w.p[1].s->channel_of(bs::Role::Pawn), actor_rpc(pawn, "ServerClientIsReadyToPhaseIn", {}), true)});
    w.settle();
    bool shown = false;
    for (const FriendBlock& block : friend_blocks(w, 1, b_before)) {
        const repl::FastArrayField* pending = friend_field(block, "PendingFriendRequests");
        if (!pending || pending->delta.changed.empty()) continue;
        FriendItem item;
        shown = shown || (friend_item_of(pending->delta.changed.front().values, true, item) && item.sender_guid == friend_words(kFriendA));
    }
    CHECK(shown, "the client is shown the pending request after the phase-in");
    // the first hand-over of the progress carries the request, so no save erases it
    w.p[1].s->gm_reset_tutorials(w.p[1].conn, "the friend test");
    const svc::Progress* saved = friend_progress_of(w.service, kFriendB);
    CHECK(saved && saved->replace_friend_requests && saved->friend_requests.size() == 1 && saved->friend_requests[0].sender_guid == kFriendA,
          "the first save after the phase-in writes the request, not an empty list");
}

AOC_REGISTER_TEST(test_a_request_while_the_target_is_connecting_is_shown_at_the_phase_in);

static void test_a_request_stored_after_the_login_read_is_taken_at_the_join() {
    std::printf("a request saved after the target's login read is taken when the session joins\n");
    FriendWorld w;
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard"))) {
        CHECK(false, "the sender joins");
        return;
    }
    // the target's login read this record before the request was saved; the service holds the request now
    const svc::Character stale = friend_character("Brannoc", kFriendB, 7, "Tank");
    svc::Character current = stale;
    svc::FriendRequest request = friend_request(0, kFriendB);
    request.sender_guid = kFriendA;
    request.sender_name = "Astra";
    current.friend_requests = {request};
    w.service.characters[kFriendB] = current;
    w.p[1].s = PlayerSession::join(w.p[1].conn, *w.fx.world, stale, identity(), world_time_at(w.t), w.p[1].error);
    CHECK(w.p[1].s != nullptr, "the target's session joins with the record its login read");
    if (!w.p[1].s) return;
    w.settle();
    CHECK(w.p[1].s->friend_requests().size() == 1 && w.p[1].s->friend_requests()[0].sender_guid == kFriendA,
          "the join takes the request the character service holds");
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    w.client_sends(w.p[1], {w.p[1].client.actor(w.p[1].s->channel_of(bs::Role::Pawn), actor_rpc(pawn, "ServerClientIsReadyToPhaseIn", {}), true)});
    w.settle();
    const auto blocks = friend_blocks(w, 1, 0);
    bool shown = false;
    for (const FriendBlock& block : blocks) {
        const repl::FastArrayField* pending = friend_field(block, "PendingFriendRequests");
        if (!pending || pending->delta.changed.empty()) continue;
        FriendItem item;
        shown = shown || (friend_item_of(pending->delta.changed.front().values, true, item) && item.sender_guid == friend_words(kFriendA));
    }
    CHECK(shown, "its first block carries the request");
}

AOC_REGISTER_TEST(test_a_request_stored_after_the_login_read_is_taken_at_the_join);

// ---- the older session's newer progress and a request in the login window ---------------------------------------------------

/// A record whose level and Adventuring XP row are newer than the record a login read before it: level 2, an XP total over the level's threshold,
/// and a finished tutorial so that the session can hand its progress over.
static svc::Character friend_join_newer_record(const svc::Character& from, const leveling::LevelCurve& curve) {
    svc::Character c = from;
    c.level = 2;
    c.experience = {{content::kGuidAdventuring, curve.xp_for(2) + 100, 0}};
    c.completed_ftues = {123};
    return c;
}

/// The Adventuring XP row of a record, or nullptr.
static const svc::Experience* friend_join_adventuring(const svc::Character& c) {
    for (const svc::Experience& e : c.experience) {
        if (e.leveling_type_guid == content::kGuidAdventuring) return &e;
    }
    return nullptr;
}

/// The request `A` sends to `B` while `B` is still connecting must be saved with `B`'s two lists only, so the level and XP the older session handed
/// over (still queued in the saver) are the ones the character service keeps.
static void test_a_request_to_a_connecting_target_keeps_the_queued_newer_progress() {
    std::printf("a request to a connecting target keeps the level and XP its older session handed over, still queued in the saver\n");
    FriendStoreApi api;
    FriendStoreApi::Gate gate;
    api.gate = &gate;
    api.gate_id = kFriendC;
    api.gate_progress = false;   // (the worker is held on another character's position, so the target's progress waits)
    CharacterSaver::Config config;
    config.max_attempts = 1;
    config.destructor_wait = std::chrono::milliseconds(500);
    CharacterSaver saver(api, config);
    {
        FriendWorld w;
        w.fx.world->set_position_sink(&saver);
        const leveling::LevelCurve* curve = w.fx.world->level_curve();
        if (!curve) {
            CHECK(false, "the fixture has the Adventuring curve");
            gate.release();
            return;
        }
        const svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
        const svc::Character stale = friend_character("Brannoc", kFriendB, 7, "Tank");   // (the login read this before the older session's save)
        const svc::Character newer = friend_join_newer_record(stale, *curve);
        api.characters[kFriendA] = a;
        api.characters[kFriendB] = newer;
        saver.save(kFriendC, svc::Position{});
        if (!gate.wait_entered()) {
            CHECK(false, "(the saver's worker is held)");
            gate.release();
            return;
        }
        if (!w.enter(0, a) || !w.enter(1, newer)) {
            CHECK(false, "the two sessions join");
            gate.release();
            return;
        }
        w.p[1].s->gm_reset_tutorials(w.p[1].conn, "the friend test");   // (the older session hands its newer progress over; it waits in the saver)
        w.p[1].s->finish("logged in elsewhere");                        // (what the endpoint does before the new session is built)
        w.p[2].s = PlayerSession::join(w.p[2].conn, *w.fx.world, stale, identity(), world_time_at(w.t), w.p[2].error);
        if (!w.p[2].s) {
            CHECK(false, "the target's session joins from the record its login read");
            gate.release();
            return;
        }
        w.settle();
        w.request(0, friend_send_call("Brannoc"));   // (the target is still connecting: it has not phased in)
        CHECK(w.p[2].s->friend_requests().size() == 1 && w.p[2].s->friend_requests()[0].sender_guid == kFriendA, "the connecting target holds the request");
        gate.release();
        CHECK(saver.flush(std::chrono::seconds(5)), "(the saver wrote the target's progress and lists)");
        std::lock_guard<std::mutex> lock(api.mu);
        const svc::Character& kept = api.characters[kFriendB];
        const svc::Experience* adv = friend_join_adventuring(kept);
        CHECK(kept.level == 2, "the saved level is the older session's newer one, not the login record's");
        CHECK(adv && adv->xp == curve->xp_for(2) + 100 && adv->debt == 0, "and the saved experience is its newer one");
        CHECK(kept.friend_requests.size() == 1 && kept.friend_requests[0].sender_guid == kFriendA && kept.friend_requests[0].recipient_guid == kFriendB,
              "the request is in the saved lists");
    }
}

AOC_REGISTER_TEST(test_a_request_to_a_connecting_target_keeps_the_queued_newer_progress);

/// The same, with the older session's progress already written before the login read: the joining session's friend call must not write its login-era
/// level and XP over it.
static void test_a_request_to_a_connecting_target_keeps_the_written_newer_progress() {
    std::printf("a request to a connecting target keeps the level and XP its older session saved before the login read\n");
    FriendStoreApi api;
    CharacterSaver::Config config;
    config.max_attempts = 1;
    config.destructor_wait = std::chrono::milliseconds(500);
    CharacterSaver saver(api, config);
    {
        FriendWorld w;
        w.fx.world->set_position_sink(&saver);
        const leveling::LevelCurve* curve = w.fx.world->level_curve();
        if (!curve) {
            CHECK(false, "the fixture has the Adventuring curve");
            return;
        }
        const svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
        const svc::Character stale = friend_character("Brannoc", kFriendB, 7, "Tank");   // (the login read this before the older session's save)
        const svc::Character newer = friend_join_newer_record(stale, *curve);
        api.characters[kFriendA] = a;
        api.characters[kFriendB] = newer;
        if (!w.enter(0, a) || !w.enter(1, newer)) {
            CHECK(false, "the two sessions join");
            return;
        }
        w.p[1].s->gm_reset_tutorials(w.p[1].conn, "the friend test");
        w.p[1].s->finish("logged in elsewhere");
        if (!saver.flush(std::chrono::seconds(5))) {
            CHECK(false, "(the older session's progress is written before the new login)");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(api.mu);
            CHECK(api.characters[kFriendB].level == 2, "(the older session's newer level is written before the new login)");
        }
        w.p[2].s = PlayerSession::join(w.p[2].conn, *w.fx.world, stale, identity(), world_time_at(w.t), w.p[2].error);
        if (!w.p[2].s) {
            CHECK(false, "the target's session joins from the record its login read");
            return;
        }
        w.settle();
        w.request(0, friend_send_call("Brannoc"));
        CHECK(w.p[2].s->friend_requests().size() == 1 && w.p[2].s->friend_requests()[0].sender_guid == kFriendA, "the connecting target holds the request");
        CHECK(saver.flush(std::chrono::seconds(5)), "(the saver wrote the target's lists)");
        std::lock_guard<std::mutex> lock(api.mu);
        const svc::Character& kept = api.characters[kFriendB];
        const svc::Experience* adv = friend_join_adventuring(kept);
        CHECK(kept.level == 2, "the saved level is still the older session's newer one, not the login record's");
        CHECK(adv && adv->xp == curve->xp_for(2) + 100 && adv->debt == 0, "and the saved experience is still its newer one");
        CHECK(kept.friend_requests.size() == 1 && kept.friend_requests[0].sender_guid == kFriendA && kept.friend_requests[0].recipient_guid == kFriendB,
              "the request is in the saved lists");
    }
}

AOC_REGISTER_TEST(test_a_request_to_a_connecting_target_keeps_the_written_newer_progress);

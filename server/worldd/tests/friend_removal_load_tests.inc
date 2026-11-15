// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_removal_load_tests.inc
//
//  Finding 9 of the friends-list review: a removal whose other side cannot be loaded.  The removal must not happen on one side only: nothing
//  changes and nothing is saved, so the other side never keeps still-friend true after the remover lost its entry.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

static void test_a_removal_whose_other_side_cannot_be_loaded_changes_nothing() {
    std::printf("a removal whose other side cannot be loaded leaves the remover's entry and saves nothing\n");
    FriendWorld w;
    svc::Friend entry_for_b = friend_entry(0);
    entry_for_b.guid = kFriendB;
    entry_for_b.name = "Brannoc";
    entry_for_b.level = 1;
    entry_for_b.class_id = 7;
    entry_for_b.guild_name.clear();
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {entry_for_b};
    if (!w.enter(0, a)) {
        CHECK(false, "the remover joins");
        return;
    }
    // Brannoc has no session here and the character service does not know it: the other side cannot be written
    w.service.characters.erase(kFriendB);
    const size_t saves_before = w.service.saves.size();
    const auto friends_before = w.p[0].s->friends();
    w.request(0, friend_remove_call(kFriendB));
    CHECK(w.p[0].s->friends() == friends_before && w.p[0].s->friends().size() == 1,
          "the remover keeps its entry: the removal did not happen on one side only");
    CHECK(w.service.saves.size() == saves_before, "nothing was saved");
}

AOC_REGISTER_TEST(test_a_removal_whose_other_side_cannot_be_loaded_changes_nothing);

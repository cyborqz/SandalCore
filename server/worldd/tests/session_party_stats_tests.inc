// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_party_stats_tests.inc: the party's live stats (Social, seeing other players: live Health and Mana)
//
//  Registered tests of test_worldd_session.  The party frame's PlayerStats and the SocialComponent's PlayerStatsReplication items carry each member's real Health,
//  Mana, maxima, level, alive state, place and heading: on join, when a member joins or leaves, and when a value changes (the live array, about once a second per
//  session).  Read from the wire: the ClientUpdateGroup frames and the SocialComponent's fast-array deltas (the rig is session_party_tests.inc's PartyWorld).
//
//  Modelled: no recording decodes either payload, so the shapes are the game's own schema's, and the alive status and the percentages are modelled (0 alive, 1 dead;
//  the current value as the share of the maximum).
// ============================================================================

namespace {

/// One PlayerStatsReplication item as the client reads it: the member's GUID as the key, the nine stats.
struct PartyStatsItem {
    std::array<uint32_t, 4> key{};
    float                   health_percent = 0.0f, max_health = 0.0f, mana_percent = 0.0f, max_mana = 0.0f, heading = 0.0f;
    int64_t                 level = 0;
    uint64_t                alive = 0;
    repl::QuantVec          location;
    bool                    complete = false;
};

/// One PlayerStatsReplication update as the client applies it: the whole array after the delta (the client's state), and the items the delta itself carried.
struct PartyStatsUpdate {
    int32_t                     array_key = 0;
    std::vector<PartyStatsItem> items;      // the whole array after this delta
    std::vector<PartyStatsItem> changed;    // the items this delta carried
};

/// The SocialComponent subobject of a session's PlayerController, or nullptr.
static const repl::Subobject* party_stats_social(const PlayerSession& s) {
    for (const repl::Subobject& sub : s.bootstrap().player_controller().subobjects) {
        if (sub.name == "SocialComponent") return &sub;
    }
    return nullptr;
}

/// Reads one PlayerStatsReplication item (the element commands 1 to 13: the nine stats and the four key words).
static bool party_stats_item_of(const repl::ElementState& es, PartyStatsItem& out) {
    const auto number = [&](uint16_t handle, int64_t& into) {
        const auto it = es.values.find(handle);
        const int64_t* v = it == es.values.end() ? nullptr : it->second.get_if<int64_t>();
        if (!v) return false;
        into = *v;
        return true;
    };
    const auto word = [&](uint16_t handle, uint32_t& into) {
        int64_t v = 0;
        if (!number(handle, v)) return false;
        into = static_cast<uint32_t>(v);
        return true;
    };
    const auto real = [&](uint16_t handle, float& into) {
        const auto it = es.values.find(handle);
        const float* v = it == es.values.end() ? nullptr : it->second.get_if<float>();
        if (!v) return false;
        into = *v;
        return true;
    };
    const auto alive = [&](uint16_t handle, uint64_t& into) {
        const auto it = es.values.find(handle);
        const uint64_t* v = it == es.values.end() ? nullptr : it->second.get_if<uint64_t>();
        if (!v) return false;
        into = *v;
        return true;
    };
    const auto place = [&](uint16_t handle, repl::QuantVec& into) {
        const auto it = es.values.find(handle);
        const repl::QuantVec* v = it == es.values.end() ? nullptr : it->second.get_if<repl::QuantVec>();
        if (!v) return false;
        into = *v;
        return true;
    };
    if (es.values.size() != 13u) return false;
    bool ok = real(1, out.health_percent) && real(2, out.max_health) && real(3, out.mana_percent) && real(4, out.max_mana);
    ok = ok && number(5, out.level) && alive(6, out.alive) && place(7, out.location) && real(9, out.heading);
    repl::QuantVec marker;
    ok = ok && place(8, marker);
    ok = ok && word(10, out.key[0]) && word(11, out.key[1]) && word(12, out.key[2]) && word(13, out.key[3]);
    out.complete = ok;
    return ok;
}

/// The PlayerStatsReplication delta one delivered chain carries, if it carries one.
static std::optional<repl::FastArrayDelta> party_stats_delta_of(const fake::Delivered& d, const repl::Subobject& social) {
    if (d.close || d.content.bit_pos() == 0) return std::nullopt;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return std::nullopt;
    const schema::ClassSchema* cls = social.obj.schema;
    const schema::NetField* f = cls ? cls->field("PlayerStatsReplication") : nullptr;
    if (!f) return std::nullopt;
    for (const repl::BlockView& b : *blocks) {
        if (b.header.is_actor || !(b.header.object == social.guid)) continue;
        size_t pos = b.payload_pos;
        const size_t end = pos + b.payload_bits;
        if (b.header.has_rep_layout) {
            if (!repl::read_rep_block(*cls, d.content.data(), end, pos, social.obj.opts)) return std::nullopt;
        }
        if (pos >= end) return std::nullopt;
        auto fields = repl::read_fast_array_fields(*cls, d.content.data(), end, pos, social.obj.opts);
        if (!fields) return std::nullopt;
        for (const repl::FastArrayField& field : *fields) {
            if (field.field == f->index) return field.delta;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/// Every PlayerStatsReplication update player `i`'s client was sent from `from` on, oldest first, each with the array as the client holds it after that delta.  The deltas
/// before `from` are applied too (the client's array is the sum of all of them), they are just not returned.
static std::vector<PartyStatsUpdate> party_stats_updates(PartyWorld& w, size_t i, size_t from) {
    std::vector<PartyStatsUpdate> out;
    const repl::Subobject* social = w[i].s ? party_stats_social(*w[i].s) : nullptr;
    if (!social || !social->obj.schema) return out;
    const schema::ClassSchema* cls = social->obj.schema;
    const schema::NetField* f = cls->field("PlayerStatsReplication");
    if (!f) return out;
    repl::FastArrays arrays;
    repl::FastArrayState* state = arrays.get(*cls, f->index);
    if (!state) return out;
    for (size_t k = 0; k < w[i].got.size(); ++k) {
        const auto delta = party_stats_delta_of(w[i].got[k], *social);
        if (!delta) continue;
        PartyStatsUpdate u;
        for (const repl::FastArrayDelta::Changed& c : delta->changed) {
            PartyStatsItem parsed;
            party_stats_item_of(c.values, parsed);
            u.changed.push_back(std::move(parsed));
        }
        if (!repl::apply_fast_array_delta(*state, *delta)) return out;
        if (k < from) continue;
        u.array_key = state->array_key();
        for (const repl::FastArrayItem& item : state->items()) {
            PartyStatsItem parsed;
            party_stats_item_of(item.values, parsed);
            u.items.push_back(std::move(parsed));
        }
        out.push_back(std::move(u));
    }
    return out;
}

/// The last PlayerStatsReplication update player `i`'s client was sent from `from` on (not there: an empty one).
static PartyStatsUpdate last_party_stats(PartyWorld& w, size_t i, size_t from) {
    const auto updates = party_stats_updates(w, i, from);
    return updates.empty() ? PartyStatsUpdate{} : updates.back();
}

/// The item of `guid` in an update, or nullptr.
static const PartyStatsItem* item_of(const PartyStatsUpdate& u, const std::array<uint32_t, 4>& guid) {
    for (const PartyStatsItem& it : u.items) {
        if (it.key == guid) return &it;
    }
    return nullptr;
}

/// The item of `guid` among the items one delta carried, or nullptr.
static const PartyStatsItem* changed_item_of(const PartyStatsUpdate& u, const std::array<uint32_t, 4>& guid) {
    for (const PartyStatsItem& it : u.changed) {
        if (it.key == guid) return &it;
    }
    return nullptr;
}

/// True when the item carries the expected stats (the shares within `share_tolerance`: a regeneration tick may have passed since the frame was built).
static bool item_is(const PartyStatsItem& it, const PartyStatsExpected& e, float share_tolerance, std::string* why) {
    if (!it.complete) {
        *why = "an item does not read";
        return false;
    }
    if (!party_near(it.health_percent, e.health_percent, share_tolerance) || !party_near(it.max_health, e.max_health, 1e-3f) ||
        !party_near(it.mana_percent, e.mana_percent, share_tolerance) || !party_near(it.max_mana, e.max_mana, 1e-3f)) {
        *why = "the health or mana values differ";
        return false;
    }
    if (it.level != e.level || it.alive != e.alive || !(it.location == e.location) || !party_near(it.heading, e.heading, 1e-3f)) {
        *why = "the level, alive state, place or heading differs";
        return false;
    }
    return true;
}

/// Steps `w` for `ms` (a step is 20 ms).
static void party_run(PartyWorld& w, int ms) {
    for (int done = 0; done < ms; done += 20) w.step();
}

/// Sets a member's Health to `value` (the game's own event path).
static bool party_hurt(PartyWorld& w, size_t i, float value) { return w[i].s && w[i].s->set_current_value(content::kGuidHealth, value); }

/// The current value of a member's stat (0 when the session has none).
static float party_stat(PartyWorld& w, size_t i, int64_t record) {
    const stats::CharacterStats* cs = w[i].s ? w[i].s->character_stats() : nullptr;
    const stats::StatValue* v = cs ? cs->find(record) : nullptr;
    return v ? v->value : 0.0f;
}

}  // namespace

// ---- the frame on join ---------------------------------------------------------------------------------------------------------------

static void test_the_party_frame_carries_each_members_real_stats() {
    std::printf("the party frame carries each member's real Health, Mana, maxima, level, alive state, place and heading\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[2].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    // p3 is invited, and the invitation waits: the next frame is the accept's
    const size_t before_request = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3");
    w.settle();
    const auto requests = party_requests_in(w[2].got, pc_cls, before_request);
    CHECK(requests.size() == 1, "p3's client gets the invitation");
    if (requests.size() != 1) return;
    // p2 is hurt and p3 accepts at the same instant: the frame is built with p2's value as it is set, before any regeneration tick
    const float max_h = party_stat(w, 1, content::kGuidMaxHealth);
    const float hurt = max_h - 30.0f;
    CHECK(max_h > 30.0f && party_hurt(w, 1, hurt), "(p2 is hurt)");
    const size_t before = w[0].got.size();
    CHECK(party_accept(w, 2, requests[0].group_guid, requests[0].inviter), "p3 accepts");
    party_run(w, 200);
    const auto frames = party_frames_in(w[0].got, pc_cls, before);
    CHECK(!frames.empty(), "p1 gets the three-member frame");
    if (frames.empty()) return;
    const PartyFrame& frame = frames.back();
    std::string why;
    CHECK(frame_lists(frame, {"p1", "p2", "p3"}, w.guid_of(0), &why), ("the frame lists all three: " + why).c_str());
    CHECK(frame.members.size() == 3, "(three rows)");
    if (frame.members.size() != 3) return;
    const float share_tolerance = 2.0f * party_stat(w, 1, content::kGuidHealthRegeneration) / max_h + 1e-6f;   // (a tick may have passed since the frame)
    const PartyMemberFrame& first = frame.members[0];
    const PartyMemberFrame& second = frame.members[1];
    const PartyMemberFrame& third = frame.members[2];
    const PartyStatsExpected e0 = party_stats_expected(*w[0].s), e2 = party_stats_expected(*w[2].s);
    CHECK(first.max_health == e0.max_health && party_near(first.health_percent, e0.health_percent, share_tolerance) && first.level == e0.level &&
              first.alive_status == e0.alive && first.location == e0.location && first.heading == e0.heading,
          "the first row has p1's real maximum and current Health, its level, alive state, place and heading");
    CHECK(second.max_health == max_h && second.health_percent == hurt / max_h, "the second row has p2's maximum and the Health it was just set to, not zero");
    CHECK(second.level == e0.level && second.alive_status == 0, "...its level and alive state");
    CHECK(third.max_health == e2.max_health && party_near(third.health_percent, e2.health_percent, share_tolerance) && third.location == e2.location,
          "the third row has p3's real values");
    CHECK(second.mana_percent > 0.0f && second.mana_percent <= 1.0f && second.max_mana > 0.0f, "(a member's Mana share and maximum are real)");
    // the same frame reaches the member
    const PartyFrame member_frame = last_party_frame(w[2].got, pc_cls);
    CHECK(member_frame.present && member_frame.members.size() == 3 && member_frame.members[1].max_health == max_h, "p3's own frame carries the same values");
}
AOC_REGISTER_TEST(test_the_party_frame_carries_each_members_real_stats);

// ---- the live array on join and on a change ------------------------------------------------------------------------------------------

static void test_the_party_live_stats_carry_each_member_and_follow_a_change() {
    std::printf("the SocialComponent's PlayerStatsReplication carries one item per member, keyed by his character GUID, and follows a change\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    const size_t before = w[0].got.size();
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    party_run(w, 1200);   // (the array's first items are one update a second at most)
    const PartyStatsUpdate joined = last_party_stats(w, 0, before);
    CHECK(joined.items.size() == 2, "p1's client is sent the array with one item per member");
    if (joined.items.size() != 2) return;
    std::string why;
    const PartyStatsItem* one = item_of(joined, w.guid_of(0));
    const PartyStatsItem* two = item_of(joined, w.guid_of(1));
    CHECK(one && two, "the items are keyed by the two characters' GUIDs");
    if (!one || !two) return;
    CHECK(item_is(*one, party_stats_expected(*w[0].s), 1e-6f, &why), ("p1's item carries his stats: " + why).c_str());
    CHECK(item_is(*two, party_stats_expected(*w[1].s), 1e-6f, &why), ("p2's item carries his: " + why).c_str());
    // the member's own client has its array too
    const PartyStatsUpdate joined_b = last_party_stats(w, 1, before);
    CHECK(joined_b.items.size() == 2 && item_of(joined_b, w.guid_of(0)) && item_of(joined_b, w.guid_of(1)), "p2's client is sent the same two items");
    // p2 is hurt, then healed to full: the delta carries p2's item with the share it has now, and not p1's (which did not change)
    const float max_h = party_stat(w, 1, content::kGuidMaxHealth);
    CHECK(party_hurt(w, 1, max_h - 40.0f), "(p2 is hurt)");
    party_run(w, 1200);
    const size_t from = w[0].got.size();
    CHECK(party_hurt(w, 1, max_h), "(p2 is healed to full)");
    party_run(w, 1200);
    const auto updates = party_stats_updates(w, 0, from);
    CHECK(!updates.empty(), "p1's client is sent an array update after the change");
    if (updates.empty()) return;
    const PartyStatsItem* changed = changed_item_of(updates.back(), w.guid_of(1));
    CHECK(changed && changed->health_percent == 1.0f && changed->max_health == max_h, "p2's item carries his new Health share (full) and his maximum");
    CHECK(!changed_item_of(updates.back(), w.guid_of(0)), "...and p1's item is not in the delta (it did not change)");
}
AOC_REGISTER_TEST(test_the_party_live_stats_carry_each_member_and_follow_a_change);

// ---- a member joins and leaves -------------------------------------------------------------------------------------------------------

static void test_a_member_who_joins_and_leaves_changes_the_party_stats() {
    std::printf("a member who joins adds his item and his row, and one who leaves takes them away\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[2].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    party_run(w, 1200);
    const size_t before_join = w[0].got.size();
    const size_t before_request = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3");
    w.settle();
    const auto requests = party_requests_in(w[2].got, pc_cls, before_request);
    CHECK(requests.size() == 1, "p3's client gets the invitation");
    if (requests.size() != 1) return;
    CHECK(party_accept(w, 2, requests[0].group_guid, requests[0].inviter), "p3 accepts");
    party_run(w, 1200);
    const PartyStatsUpdate three = last_party_stats(w, 0, before_join);
    CHECK(three.items.size() == 3 && item_of(three, w.guid_of(2)), "p1's array now holds p3's item too");
    const PartyFrame frame3 = last_party_frame(w[0].got, pc_cls);
    std::string why;
    CHECK(frame_lists(frame3, {"p1", "p2", "p3"}, w.guid_of(0), &why), ("the frame lists all three: " + why).c_str());
    CHECK(frame3.members.size() == 3 && frame3.members[2].max_health == party_stats_expected(*w[2].s).max_health, "...with p3's real maximum");
    // p3 leaves: the array loses its item, the frame its row
    const size_t before_leave = w[0].got.size();
    CHECK(party_leave(w, 2), "p3 leaves");
    party_run(w, 1200);
    const PartyStatsUpdate two = last_party_stats(w, 0, before_leave);
    CHECK(two.items.size() == 2 && !item_of(two, w.guid_of(2)), "p1's array drops p3's item");
    const auto frames = party_frames_in(w[0].got, pc_cls, before_leave);
    CHECK(frames.size() == 1 && frames.back().members.size() == 2, "p1's frame drops p3's row");
    if (!frames.empty()) CHECK(frame_lists(frames.back(), {"p1", "p2"}, w.guid_of(0), &why), ("the frame lists the two left: " + why).c_str());
}
AOC_REGISTER_TEST(test_a_member_who_joins_and_leaves_changes_the_party_stats);

// ---- the rate of a party of eight ----------------------------------------------------------------------------------------------------

static void test_the_party_stats_rate_holds_for_a_party_of_eight() {
    std::printf("a party of eight sends its live stats about once a second per client, with every changed member in one update\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 8), "eight players enter the world");
    if (!w[0].s || !w[7].s) return;
    for (size_t i = 1; i <= 7; ++i) {
        const size_t before = w[i].got.size();
        CHECK(party_invite(w, 0, w[i].s->name()), ("p1 invites p" + std::to_string(i + 1)).c_str());
        w.settle();
        const auto requests = party_requests_in(w[i].got, pc_cls, before);
        CHECK(requests.size() == 1 && party_accept(w, i, requests[0].group_guid, requests[0].inviter), ("p" + std::to_string(i + 1) + " accepts").c_str());
        w.settle();
    }
    party_run(w, 1200);   // (the join's own updates are done)
    // every member's Mana is set below its maximum: a change a second for each of the eight
    for (size_t i = 0; i < 8; ++i) {
        const float max_m = party_stat(w, i, content::kGuidMaxMana), regen = party_stat(w, i, content::kGuidManaRegeneration);
        CHECK(w[i].s->set_current_value(content::kGuidMana, std::max(0.0f, max_m - regen * 4.0f)), ("p" + std::to_string(i + 1) + "'s Mana is set below its maximum").c_str());
    }
    std::vector<size_t> from(8);
    for (size_t i = 0; i < 8; ++i) from[i] = w[i].got.size();
    party_run(w, 5000);
    bool capped = true, carries_all = true, everyone = true;
    size_t most = 0;
    for (size_t i = 0; i < 8; ++i) {
        const auto updates = party_stats_updates(w, i, from[i]);
        most = std::max(most, updates.size());
        capped = capped && updates.size() >= 3 && updates.size() <= 6;   // five seconds, one update a second at most
        std::set<std::array<uint32_t, 4>> keys;
        for (const PartyStatsUpdate& u : updates) {
            if (u.changed.size() > 8) carries_all = false;
            for (const PartyStatsItem& it : u.changed) keys.insert(it.key);
        }
        everyone = everyone && keys.size() == 8;   // every member's change was carried, in one update or another
    }
    CHECK(capped, "each of the eight clients is sent three to six live-stat updates in five seconds (not one a tick)");
    CHECK(carries_all, "...and no update carries more than the eight members");
    CHECK(everyone, "...with every member's change carried");
    CHECK(most <= 6, "the busiest client got no more than six updates in five seconds");
}
AOC_REGISTER_TEST(test_the_party_stats_rate_holds_for_a_party_of_eight);

// ---- two parties side by side --------------------------------------------------------------------------------------------------------

static void test_two_parties_side_by_side_keep_their_own_stats() {
    std::printf("two parties side by side carry only their own members in their frames and arrays\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 4), "four players enter the world");
    if (!w[0].s || !w[3].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form the first party");
    CHECK(party_pair(w, 2, 3, pc_cls), "p3 and p4 form the second");
    party_run(w, 1200);
    const PartyStatsUpdate first = last_party_stats(w, 0, 0);
    const PartyStatsUpdate second = last_party_stats(w, 2, 0);
    CHECK(first.items.size() == 2 && item_of(first, w.guid_of(0)) && item_of(first, w.guid_of(1)) && !item_of(first, w.guid_of(2)) && !item_of(first, w.guid_of(3)),
          "the first party's array holds its two members only");
    CHECK(second.items.size() == 2 && item_of(second, w.guid_of(2)) && item_of(second, w.guid_of(3)) && !item_of(second, w.guid_of(0)) && !item_of(second, w.guid_of(1)),
          "the second party's array holds its own two only");
    // a change in the first party reaches its members and not the other party's
    const size_t from_first = w[1].got.size(), from_second = w[3].got.size();
    CHECK(party_hurt(w, 0, party_stat(w, 0, content::kGuidMaxHealth) - 25.0f), "(p1 is hurt)");
    party_run(w, 1200);
    CHECK(!party_stats_updates(w, 1, from_first).empty() && !party_stats_updates(w, 0, from_first).empty(), "the first party's clients are sent p1's change");
    CHECK(party_stats_updates(w, 3, from_second).empty(), "the second party's client is sent nothing of it");
}
AOC_REGISTER_TEST(test_two_parties_side_by_side_keep_their_own_stats);

// ---- a reconnect ----------------------------------------------------------------------------------------------------------------------

static void test_a_member_who_reconnects_is_seen_again_with_its_current_stats() {
    std::printf("a member who logs out leaves the party and its items, and one who comes back and joins again is seen with its current stats\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    const std::array<uint32_t, 4> p2_guid = w.guid_of(1);
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    party_run(w, 1200);
    CHECK(party_hurt(w, 1, party_stat(w, 1, content::kGuidMaxHealth) - 35.0f), "(p2 is hurt)");
    party_run(w, 1200);
    // p2 logs out: p1's frame and array lose it
    const size_t before = w[0].got.size();
    w[1].s->finish("the test");
    w[1].s.reset();
    party_run(w, 1200);
    const PartyStatsUpdate after_logout = last_party_stats(w, 0, before);
    CHECK(after_logout.items.empty(), "p1's array empties when p2 logs out (the party is down to one, so it is gone)");
    const auto frames = party_frames_in(w[0].got, pc_cls, before);
    CHECK(!frames.empty() && frames.back().present && frame_is_empty(frames.back()), "p1's frame empties when p2 logs out");
    // p2 comes back and joins a new party: its stats are its new life's
    CHECK(w.enter(1, party_record("p2", p2_guid), identity()), "(p2 logs in again)");
    if (!w[1].s) return;
    const size_t before_join = w[0].got.size();
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party again");
    party_run(w, 1200);
    const PartyStatsUpdate again = last_party_stats(w, 0, before_join);
    CHECK(again.items.size() == 2 && item_of(again, p2_guid), "p1's array holds p2's item again");
    const PartyStatsItem* p2 = item_of(again, p2_guid);
    CHECK(p2 && party_near(p2->health_percent, 1.0f), "...with the full Health of its new life, not the Health before the logout");
    const PartyFrame frame = last_party_frame(w[0].got, pc_cls);
    std::string why;
    CHECK(frame_lists(frame, {"p1", "p2"}, w.guid_of(0), &why), ("the frame lists both again: " + why).c_str());
}
AOC_REGISTER_TEST(test_a_member_who_reconnects_is_seen_again_with_its_current_stats);

// ---- the same change twice ------------------------------------------------------------------------------------------------------------

static void test_the_same_health_twice_sends_one_party_update() {
    std::printf("the same Health set twice sends one live-stat update, and nothing for the second\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    party_run(w, 1200);
    const float max_h = party_stat(w, 1, content::kGuidMaxHealth);
    CHECK(party_hurt(w, 1, max_h - 20.0f), "(p2 is hurt once)");
    party_run(w, 1200);   // (its update is sent)
    // the heal to full is a change; setting full again is not
    CHECK(party_hurt(w, 1, max_h), "(p2 is healed to full)");
    const size_t from_heal = w[0].got.size();
    party_run(w, 1200);
    const auto healed = party_stats_updates(w, 0, from_heal);
    CHECK(healed.size() == 1, "p1 is sent one live-stat update for the heal");
    if (!healed.empty()) {
        const PartyStatsItem* p2 = item_of(healed.back(), w.guid_of(1));
        CHECK(p2 && p2->health_percent == 1.0f, "...carrying the full share");
    }
    const size_t after_heal = w[0].got.size();
    CHECK(party_hurt(w, 1, max_h), "(the same value is set again)");
    party_run(w, 1200);
    CHECK(party_stats_updates(w, 0, after_heal).empty(), "...and nothing for the second");
}
AOC_REGISTER_TEST(test_the_same_health_twice_sends_one_party_update);

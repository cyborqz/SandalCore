// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_presence_emotes_tests.inc: players seeing each other's emotes (through the Endpoint, with the view passes on 0 and 3 threads)
//
//  Two players are in range of each other.  One of them types /sit: the other player's client is sent the Rest's ability item on the first player's pawn, with the export of the Rest
//  montage's path in the same chain, so that it can play the sit-down; a second /sit is sent as the same item with the montage jumped to its end section.  The player who sits is sent
//  nothing about its own pawn but what it has always been sent.
// ============================================================================
#include "movement_presence_rig.h"

static constexpr int64_t kPresenceEmoteWave = 107140329395832ll;   // Emote_Wave: /wave and /hi

/// A client's ServerPlayEmote(EmoteName): a call of the PlayerController.
static BunchWriter presence_emote_bunch(const std::string& word) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerPlayEmote");
    repl::RpcParams params;
    BunchWriter out;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "EmoteName", repl::Value::str(word))) {
        std::printf("  [FAIL] the parameter of ServerPlayEmote does not fit\n");
        return out;
    }
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(pc_cls, *rpc, params, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] ServerPlayEmote does not write\n");
        return out;
    }
    repl::BlockHeader header;   // the actor's own: no RepLayout block
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// One ReplicatedActiveAbilities element a client was sent for another player's pawn.
struct PresenceSeenItem {
    int64_t  ability = 0;
    uint64_t montage = 0;
    bool     jump = false;
    float    position = 0;
    int32_t  counter = 0;
    bool     with_exports = false;   // the chain that carried it held an export
};

/// The ability items `viewer` was sent on the pawns of other players (a channel past the viewer's own four), in the order they came; `ability_object` is the ObjectId of the AbilityComponent
/// of the player whose pawn it is.  `deleted` counts the items the updates removed.
static std::vector<PresenceSeenItem> presence_seen_items(const PresenceClient& viewer, uint64_t ability_object, size_t* deleted = nullptr) {
    std::vector<PresenceSeenItem> out;
    if (deleted) *deleted = 0;
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    for (const fake::Delivered& d : viewer.j.got) {
        if (d.channel <= 6 || d.open || d.close || d.content.bit_pos() <= 8) continue;
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) continue;
        for (const repl::BlockView& v : *views) {
            if (v.header.is_actor || v.header.object.ObjectId != ability_object) continue;
            size_t p = v.payload_pos;
            const auto fields = repl::read_fast_array_fields(cls, d.content.data(), v.payload_pos + v.payload_bits, p, repl::CodecOptions{});
            if (!fields) continue;
            for (const repl::FastArrayField& f : *fields) {
                if (std::string(cls.field(f.field)->name) != "ReplicatedActiveAbilities") continue;
                if (deleted) *deleted += f.delta.deleted.size();
                for (const auto& c : f.delta.changed) {
                    const auto val = [&](uint16_t handle) -> const repl::Value* {
                        const auto it = c.values.values.find(handle);
                        return it == c.values.values.end() ? nullptr : &it->second;
                    };
                    PresenceSeenItem s;
                    if (const repl::Value* x = val(1)) s.ability = x->get_if<int64_t>() ? *x->get_if<int64_t>() : 0;
                    if (const repl::Value* x = val(11)) s.montage = x->get_if<emit::FIntrepidNetworkGUID>() ? x->get_if<emit::FIntrepidNetworkGUID>()->ObjectId : 0;
                    if (const repl::Value* x = val(13)) s.position = x->get_if<float>() ? *x->get_if<float>() : 0.0f;
                    if (const repl::Value* x = val(14)) s.jump = x->get_if<bool>() && *x->get_if<bool>();
                    if (const repl::Value* x = val(15)) s.counter = x->get_if<int64_t>() ? static_cast<int32_t>(*x->get_if<int64_t>()) : 0;
                    s.with_exports = !d.exports.empty();
                    out.push_back(s);
                }
            }
        }
    }
    return out;
}

static void test_presence_a_sit_is_seen_by_the_other_player() {
    std::printf("players seeing each other: a /sit and a /wave are sent to the other player's client with their items, montages and exports, with 0 and 3 view threads\n");
    const char* real = std::getenv("AOC_CONTENT_DB");
    const bool real_db = real != nullptr && *real != '\0';
    char text[420];
    for (const unsigned threads : {0u, 3u}) {
        g_counter = 1;
        PresenceEndpoints e(threads, real_db);
        Rig& rig = *e.rig;
        Clock::time_point now = t0;
        std::vector<PresenceClient*> all;
        PresenceClient a(50510, "cyb"), b(50511, "ayla");
        presence_join(rig, all, a, cyb_record(), now);
        presence_join(rig, all, b, presence_record(presence_id(2), "ayla", 4), now);
        presence_phase_in(rig, all, a, now);
        presence_phase_in(rig, all, b, now);
        presence_idle(rig, all, now, 25);
        const PlayerSession* sa = rig.ep.player_at(a.addr);
        CHECK(sa != nullptr, "(cyb's session is there)");
        if (!sa) return;
        const uint64_t ability_object = sa->bootstrap().setup().guids.ability.ObjectId;
        size_t before_deleted = 0;
        const size_t before = presence_seen_items(b, ability_object, &before_deleted).size();
        std::snprintf(text, sizeof(text), "%u view thread(s): before the sit ayla's client has been sent no ability item of cyb's pawn (%zu)", threads, before);
        CHECK(before == 0, text);

        a.out.push_back(a.j.client->actor(3, presence_emote_bunch("sit"), true));
        presence_idle(rig, all, now, 12);
        std::vector<PresenceSeenItem> seen = presence_seen_items(b, ability_object);
        const auto rest_montage = bootstrap::montage_for(bootstrap::kRestMontagePath);
        std::snprintf(text, sizeof(text), "%u view thread(s): cyb sits: ayla's client is sent cyb's Rest item (%zu items), with the Rest montage and the export of its path in the chain", threads, seen.size());
        CHECK(seen.size() == 1 && seen[0].ability == content::kGuidRest && rest_montage && seen[0].montage == rest_montage->guid.ObjectId && seen[0].with_exports && !seen[0].jump && seen[0].counter == 0, text);

        a.out.push_back(a.j.client->actor(3, presence_emote_bunch("SIT"), true));
        presence_idle(rig, all, now, 12);
        seen = presence_seen_items(b, ability_object);
        std::snprintf(text, sizeof(text), "%u view thread(s): cyb stands up: ayla's client is sent the same item with the montage jumped to its end section (%zu items)", threads, seen.size());
        CHECK(seen.size() == 2 && seen[1].ability == content::kGuidRest && seen[1].jump && seen[1].position == 5.82606792f && seen[1].counter == 1, text);
        CHECK(!rig.ep.player_at(a.addr)->abilities_failed(), "(cyb's abilities are on)");
        // a /wave (its ability is in the real content.db only): the item names the Wave02 montage and the chain that carries it exports the montage's path
        if (real_db) {
            a.out.push_back(a.j.client->actor(3, presence_emote_bunch("wave"), true));
            presence_idle(rig, all, now, 12);
            seen = presence_seen_items(b, ability_object);
            std::snprintf(text, sizeof(text), "%u view thread(s): cyb waves: ayla's client is sent cyb's Emote_Wave item with a montage and an export in the same chain (%zu items)", threads, seen.size());
            CHECK(seen.size() == 3 && seen[2].ability == kPresenceEmoteWave && seen[2].montage != 0 && seen[2].with_exports, text);
        }
        // cyb's own client is sent its own item as ever: one Rest item on its own pawn, and none of the other player's
        std::snprintf(text, sizeof(text), "%u view thread(s): ayla sat nothing: cyb's client has been sent no ability item of ayla's pawn", threads);
        const uint64_t ayla_ability = rig.ep.player_at(b.addr)->bootstrap().setup().guids.ability.ObjectId;
        CHECK(presence_seen_items(a, ayla_ability).empty(), text);
    }
}
AOC_REGISTER_TEST(test_presence_a_sit_is_seen_by_the_other_player);

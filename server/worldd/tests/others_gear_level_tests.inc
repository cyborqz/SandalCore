// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/others_gear_level_tests.inc: other players see a player's gear, weapon set and level (Social, seeing other players: gear and level changes)
//
//  A player (cyb) and one to three watchers (tests/mounts_ride_others_rig.h) in one world: the player equips and removes items, swaps weapon sets and gains a level; what a watcher's
//  client is sent is read chain by chain (the update of the pawn the watchers hold, and the level-up call).  Also: a watcher who comes into range after the changes, a relog, the same item
//  twice, two players side by side, and a change while the player rides.
//
//  Modelled: no recording shows another player's pawn receive these, so the shape is the owner's own reply's (the equipment's HeldItems, appearance and stats; the weapon flag; the
//  level-up call), kept current on the pawn the watchers hold.  The inactive weapon pair's back placement is animation, not replicated data.
// ============================================================================

/// The values a chain carries, by object (0: the actor), with the fast arrays: the RepLayout block is read on its own and then the field loop (a fast array's delta applied to an
/// empty array: its items and its ArrayReplicationKey).  A block that does not read is left out.  (The panels' reading, as test_worldd_items_session does it, rather than
/// load_object_state's all-or-nothing read: what a player's pawn carries is read for the parts these tests look at.)
static States parse_states(const fake::Delivered& d, const repl::Actor& actor) {
    States out;
    const size_t start = d.open ? new_actor_at(d.content).end : 0;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), start);
    if (!blocks) return out;
    for (const repl::BlockView& b : *blocks) {
        const schema::ClassSchema* cls = nullptr;
        uint64_t id = 0;
        if (b.header.is_actor) {
            cls = actor.obj.schema;
        } else {
            const repl::Subobject* sub = actor.find(b.header.object);
            cls = sub ? sub->obj.schema : nullptr;
            id = b.header.object.ObjectId;
        }
        if (!cls) continue;
        repl::ObjectState st;
        st.schema = cls;
        st.opts = repl::default_block_options();
        size_t p = b.payload_pos;
        const size_t end = b.payload_pos + b.payload_bits;
        if (b.header.has_rep_layout) {
            auto rep = repl::read_rep_block(*cls, d.content.data(), end, p);
            if (!rep) continue;
            st.state = std::move(*rep);
        }
        if (p < end) {
            const auto fields = repl::read_fast_array_fields(*cls, d.content.data(), end, p);
            if (fields) {
                for (const repl::FastArrayField& f : *fields) {
                    repl::FastArrayState* arr = st.arrays.get(*cls, f.field);
                    if (!arr || !repl::apply_fast_array_delta(*arr, f.delta)) break;
                }
            }
        }
        out.emplace(id, std::move(st));
    }
    return out;
}

/// The fast array `field` of the object of class `cls` that one chain carries: the items it wrote and the ArrayReplicationKey it moves to.  A chain that does not carry the field: nullopt.
struct SentArray {
    int32_t                          key = 0;
    std::vector<repl::FastArrayItem> items;
};

static std::optional<SentArray> sent_array(const fake::Delivered& d, const repl::Actor& actor, const char* cls, const char* field) {
    for (const auto& [id, o] : parse_states(d, actor)) {
        if (!o.schema || std::string(o.schema->name) != cls) continue;
        const schema::NetField* f = o.schema->field(field);
        const repl::FastArrayState* arr = f ? o.arrays.find(f->index) : nullptr;
        if (!arr) continue;
        SentArray out;
        out.key = arr->array_key();
        for (const repl::FastArrayItem& it : arr->items()) out.items.push_back(it);
        return out;
    }
    return std::nullopt;
}

/// The HeldItems a chain carries, by equip slot: the item record (0: nothing).
static std::map<int, int64_t> held_slots(const SentArray& a) {
    std::map<int, int64_t> out;
    for (const repl::FastArrayItem& it : a.items) {
        const auto slot = it.values.values.find(1), record = it.values.values.find(2);
        if (slot == it.values.values.end() || record == it.values.values.end()) continue;
        const auto* s = slot->second.get_if<uint64_t>();
        const auto* r = record->second.get_if<int64_t>();
        if (s && r) out[static_cast<int>(*s)] = *r;
    }
    return out;
}

/// The item record a HeldItems map has for `slot` (0: none).
static int64_t record_in(const std::map<int, int64_t>& held, int slot) {
    const auto it = held.find(slot);
    return it == held.end() ? 0 : it->second;
}

/// The array value `path` of the object `object` that a chain carries; nullopt when it carries none.
static std::optional<repl::ArrayValue> array_value_in(const States& st, uint64_t object, const char* path) {
    const repl::Value* v = value_in(st, object, path);
    if (!v) return std::nullopt;
    if (const auto* a = v->get_if<repl::ArrayValue>()) return *a;
    return std::nullopt;
}

/// One AppearanceIDs element as a chain writes it: the item record and its equipment slot.
struct Look {
    uint64_t record = 0;
    uint8_t  slot = 0;
};
static std::optional<Look> look_at(const repl::ArrayValue& a, size_t index) {
    if (index >= a.elements.size()) return std::nullopt;
    const auto it = a.elements[index].values.find(1);
    if (it == a.elements[index].values.end()) return std::nullopt;
    if (const auto* id = it->second.get_if<repl::AppearanceId>()) return Look{id->record_guid, id->item_equip_slot};
    return std::nullopt;
}

/// An object value of the registry's actor itself (not of a chain), for a value the watchers are shown: e.g. the attachment of the pawn they hold.
static std::optional<emit::FIntrepidNetworkGUID> actor_object(const repl::Actor& actor, const char* path) {
    if (!actor.obj.schema) return std::nullopt;
    const repl::Value* v = actor.obj.state.get(*actor.obj.schema, path);
    if (!v) return std::nullopt;
    if (const auto* g = v->get_if<emit::FIntrepidNetworkGUID>()) return *g;
    return std::nullopt;
}

/// ServerSetUseRangedWeapon (the client's set swap) on the pawn.
static BunchWriter ranged_rpc(bool ranged) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerSetUseRangedWeapon");
    const auto params = rpc ? repl::set_use_ranged_weapon_params(*rpc, ranged) : std::nullopt;
    return params ? actor_rpc(pawn, "ServerSetUseRangedWeapon", *params) : BunchWriter();
}

/// The BroadcastPlayLevelUpEffects calls of the chains a watcher got on `channel` since `from`: (Category, InLevel).
static std::vector<std::pair<int64_t, int64_t>> level_up_calls(const Harness& watcher, uint32_t channel, size_t from) {
    std::vector<std::pair<int64_t, int64_t>> out;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close) continue;
        for (const repl::RpcCall& c : calls_of(d, "PlayerPawn_C")) {
            if (!c.rpc || std::string(c.rpc->name) != "BroadcastPlayLevelUpEffects") continue;
            const auto cat = c.params.values.find(1), level = c.params.values.find(2);
            const auto* a = cat != c.params.values.end() ? cat->second.get_if<int64_t>() : nullptr;
            const auto* b = level != c.params.values.end() ? level->second.get_if<int64_t>() : nullptr;
            out.emplace_back(a ? *a : -1, b ? *b : -1);
        }
    }
    return out;
}

/// The chains a watcher got on `channel` since `from` that carry the fast array `field` of the object of class `cls`, with the array as each wrote it.
static std::vector<std::pair<const fake::Delivered*, SentArray>> arrays_on(const Harness& watcher, uint32_t channel, const repl::Actor& actor, size_t from, const char* cls,
                                                                          const char* field) {
    std::vector<std::pair<const fake::Delivered*, SentArray>> out;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close) continue;
        if (auto a = sent_array(d, actor, cls, field)) out.emplace_back(&d, std::move(*a));
    }
    return out;
}

/// The bag slot an instance is in (-1: not in the bag).
static int bag_slot_of(const PlayerSession& s, const std::string& guid) {
    for (int slot = 0; slot < items::kBackpackSlots; ++slot) {
        const svc::InventoryItem* it = s.inventory().at(Bag, slot);
        if (it && it->instance_guid == guid) return slot;
    }
    return -1;
}

/// A character of its own whose kit is its own too (the same records, other instance guids), beside the first player.
static svc::Character gear_record(int n, const char* name) {
    svc::Character c = other_record(other_id(n), name, 10 + n);
    for (svc::InventoryItem& it : c.items) it.instance_guid[0] = static_cast<char>('b' + n);
    return c;
}

// ---- an item is taken off and put on -----------------------------------------------------------------------------------------------------------

static void test_others_see_a_players_gear_change() {
    std::printf("other players see a player's gear change: one update with the look, the 23 held items and the stat deltas, and one for the item put back\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t base_id = id_of_component(*pawn, "UPlayerInfo");
    const uint64_t look_id = id_of_component(*pawn, "UCharacterAppearanceComponent");
    const fake::Delivered* open = open_on(b, pch);
    CHECK(open != nullptr, "(the watcher's client has its open)");
    if (!open) return;
    const auto held0 = sent_array(*open, pawn_copy, "UPlayerInfo", "HeldItems");
    const States pre = parse_states(*open, pawn_copy);
    const auto look0 = array_value_in(pre, look_id, "AppearanceIDs");
    CHECK(held0 && held0->items.size() == 23, "the open carries the 23 held items");
    CHECK(look0 && look0->elements.size() == 4, "...and the four appearance pieces (helmet, chest, legs, feet)");
    if (!held0 || !look0) return;
    const int32_t key0 = held0->key;
    const auto held_before = held_slots(*held0);
    CHECK(record_in(held_before, 15) == fi::kScepter && record_in(held_before, 17) == fi::kBook, "(the scepter and the book are in the held slots)");
    const auto chest = a.s->inventory().at(Worn, 2);
    const auto book = a.s->inventory().at(Worn, 17);
    CHECK(chest && chest->item_record_id == fi::kChest && book && book->item_record_id == fi::kBook, "(the chest and the ranged book are worn)");
    if (!chest || !book) return;
    const std::string chest_guid = chest->instance_guid, book_guid = book->instance_guid;
    // the chest off: the appearance, the held items (all 23 marked) and the stats in one update
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, chest_guid), true)});
    a.run(milliseconds(200));
    CHECK(!a.s->inventory().at(Worn, 2), "(the chest is off)");
    const auto off = arrays_on(b, pch, pawn_copy, from, "UPlayerInfo", "HeldItems");
    CHECK(off.size() == 1, "the watcher was sent one update of the pawn with the held items: once for the change");
    if (off.size() != 1) return;
    CHECK(off[0].second.items.size() == 23 && off[0].second.key == key0 + 23, "all 23 held items are marked and the array's key moved by 23, as the owner's own reply");
    const auto slots = held_slots(off[0].second);
    CHECK(record_in(slots, 15) == fi::kScepter && record_in(slots, 17) == fi::kBook, "the weapons in the held slots stay (the chest and the helmet are not held items: the appearance list shows them)");
    const States st = parse_states(*off[0].first, pawn_copy);
    const auto look = array_value_in(st, look_id, "AppearanceIDs");
    CHECK(look && look->elements.size() == 3, "the appearance list lost the chest: three pieces now");
    if (look) {
        const auto one = look_at(*look, 1), two = look_at(*look, 2);
        CHECK(one && one->record == static_cast<uint64_t>(fi::kLegs) && one->slot == 5 && two && two->record == static_cast<uint64_t>(fi::kFeet) && two->slot == 6,
              "...the legs and the feet moved up into its place (the slots 5 and 6)");
    }
    // the stats: the pawn the watchers hold has every EveryoneProxy value the player's pawn has, and each stat item the chain carries is one of them
    const repl::Subobject* own_stats = a.s->bootstrap().pawn().find(a.s->bootstrap().setup().guids.stats);
    const world::WorldActor* after = shown_pawn(a);
    const repl::Subobject* shown_stats = after ? after->actor.find(a.s->bootstrap().setup().guids.stats) : nullptr;
    size_t total = 0, held = 0, missed = 0, wrong = 0;
    for (const char* name : {"StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}) {
        const schema::NetField* f = own_stats && own_stats->obj.schema ? own_stats->obj.schema->field(name) : nullptr;
        const repl::FastArrayState* mine = f ? own_stats->obj.arrays.find(f->index) : nullptr;
        const repl::FastArrayState* shown = f && shown_stats ? shown_stats->obj.arrays.find(f->index) : nullptr;
        if (mine) {
            for (const repl::FastArrayItem& it : mine->items()) {
                const repl::FastArrayItem* there = shown ? shown->find(it.id) : nullptr;
                ++total;
                if (there && there->values == it.values) ++held;
                else ++missed;
            }
        }
        const auto got = sent_array(*off[0].first, pawn_copy, "UAoCStatsComponent", name);
        if (got) {
            for (const repl::FastArrayItem& it : got->items) {
                const repl::FastArrayItem* owner = mine ? mine->find(it.id) : nullptr;
                wrong += !(owner && owner->values == it.values);
            }
        }
    }
    CHECK(total > 0 && held == total && missed == 0 && wrong == 0, "the pawn the watchers hold has the player's EveryoneProxy stats after the change (and every stat item the chain carries is the player's value)");
    // the ranged book off: a held slot's record goes, and the appearance list does not change
    const size_t from_book = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 17, book_guid), true)});
    a.run(milliseconds(200));
    CHECK(!a.s->inventory().at(Worn, 17), "(the ranged book is off)");
    const auto book_off = arrays_on(b, pch, pawn_copy, from_book, "UPlayerInfo", "HeldItems");
    CHECK(book_off.size() == 1, "the weapon's change is one update too");
    if (book_off.size() != 1) return;
    CHECK(book_off[0].second.items.size() == 23 && book_off[0].second.key == key0 + 46 && record_in(held_slots(book_off[0].second), 17) == 0 &&
              record_in(held_slots(book_off[0].second), 15) == fi::kScepter,
          "the book's held slot names no record now (its key 23 more), the scepter stays");
    CHECK(!array_value_in(parse_states(*book_off[0].first, pawn_copy), look_id, "AppearanceIDs"), "a weapon is not in the appearance list: no look update for it");
    // and the chest back on
    const int bag_slot = bag_slot_of(*a.s, chest_guid);
    CHECK(bag_slot >= 0, "(the chest is in the bag)");
    if (bag_slot < 0) return;
    const size_t from2 = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(3, bag_slot, chest_guid), true)});
    a.run(milliseconds(200));
    CHECK(a.s->inventory().at(Worn, 2) != nullptr, "(the chest is worn again)");
    const auto on = arrays_on(b, pch, pawn_copy, from2, "UPlayerInfo", "HeldItems");
    CHECK(on.size() == 1, "the second change is one update too");
    if (on.size() != 1) return;
    CHECK(on[0].second.key == key0 + 69 && record_in(held_slots(on[0].second), 17) == 0, "...its key 23 more, the book's slot still empty");
    const auto look_back = array_value_in(parse_states(*on[0].first, pawn_copy), look_id, "AppearanceIDs");
    CHECK(look_back && look_back->elements.size() == 4, "the appearance list is four pieces again");
    if (look_back) {
        const auto one = look_at(*look_back, 1);
        CHECK(one && one->record == static_cast<uint64_t>(fi::kChest) && one->slot == 2, "...the chest in its old place (slot 2)");
    }
}
AOC_REGISTER_TEST(test_others_see_a_players_gear_change);

// ---- the weapon set in hand ----------------------------------------------------------------------------------------------------------------------

static void test_others_see_a_players_weapon_swap() {
    std::printf("other players see a player's weapon swap: the ranged flag once, and nothing of the held items; the same set again is nothing\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat_id = id_of_component(*pawn, "UPlayerCombatInfo");
    const uint64_t look_id = id_of_component(*pawn, "UCharacterAppearanceComponent");
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, ranged_rpc(true), true)});
    a.run(milliseconds(250));
    CHECK(a.s->using_ranged_weapon() && a.s->weapon_swaps() == 1, "(the player is on the ranged set)");
    std::vector<int64_t> flags;
    size_t gear = 0;
    for (size_t i = from; i < b.got.size(); ++i) {
        const fake::Delivered& d = b.got[i];
        if (d.channel != pch || d.close) continue;
        const States st = parse_states(d, pawn_copy);
        if (const auto f = number_in(st, combat_id, "bIsUsingRangedWeapon")) flags.push_back(*f);
        if (sent_array(d, pawn_copy, "UPlayerInfo", "HeldItems") || array_value_in(st, look_id, "AppearanceIDs")) ++gear;
    }
    CHECK(flags.size() == 1 && flags[0] == 1, "the watcher is sent the ranged flag once, as true");
    CHECK(gear == 0, "no item moved: no held items or appearance update for the swap");
    // the same set again: the session changes nothing, and nobody is sent anything
    const size_t from2 = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, ranged_rpc(true), true)});
    a.run(milliseconds(200));
    CHECK(a.s->weapon_swaps() == 1, "(the same set again changes nothing)");
    size_t sent_after = 0;
    for (size_t i = from2; i < b.got.size(); ++i) sent_after += b.got[i].channel == pch && !b.got[i].close ? 1 : 0;
    CHECK(sent_after == 0, "...and the watcher is sent nothing");
    // back to the melee set
    const size_t from3 = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, ranged_rpc(false), true)});
    a.run(milliseconds(250));
    std::vector<int64_t> back;
    for (size_t i = from3; i < b.got.size(); ++i) {
        const fake::Delivered& d = b.got[i];
        if (d.channel != pch || d.close) continue;
        if (const auto f = number_in(parse_states(d, pawn_copy), combat_id, "bIsUsingRangedWeapon")) back.push_back(*f);
    }
    CHECK(a.s->weapon_swaps() == 2 && back.size() == 1 && back[0] == 0, "back on the melee set the watcher is sent the flag once, as false");
}
AOC_REGISTER_TEST(test_others_see_a_players_weapon_swap);

// ---- a level ---------------------------------------------------------------------------------------------------------------------------------------

static void test_others_see_a_players_level_up() {
    std::printf("other players see a player gain a level: the new Level once and the level-up effect once, the owner's own copy still one\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(a.s->progress().level == 1 && a.s->progress().xp == 0, "(the player is level 1 with 0 XP)");
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t base_id = id_of_component(*pawn, "UPlayerInfo");
    const size_t from = b.got.size(), from_own = a.got.size();
    const PlayerSession::GmXpResult r = a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    CHECK(r.ok && r.leveled && r.level_after == 2, "(350 XP crosses level 2, the curve's 300)");
    a.run(milliseconds(300));
    std::vector<int64_t> levels;
    for (size_t i = from; i < b.got.size(); ++i) {
        const fake::Delivered& d = b.got[i];
        if (d.channel != pch || d.close) continue;
        if (const auto v = number_in(parse_states(d, pawn_copy), base_id, "Level")) levels.push_back(*v);
    }
    CHECK(levels.size() == 1 && levels[0] == 2, "the watcher is sent the new Level once (2)");
    // the stats the new level changes: the pawn the watchers hold has the player's EveryoneProxy values, and each stat item the chains carry is one of them
    const repl::Subobject* own_stats = a.s->bootstrap().pawn().find(a.s->bootstrap().setup().guids.stats);
    const world::WorldActor* after = shown_pawn(a);
    const repl::Subobject* shown_stats = after ? after->actor.find(a.s->bootstrap().setup().guids.stats) : nullptr;
    size_t total = 0, held = 0, missed = 0, wrong = 0;
    for (const char* name : {"StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}) {
        const schema::NetField* f = own_stats && own_stats->obj.schema ? own_stats->obj.schema->field(name) : nullptr;
        const repl::FastArrayState* mine = f ? own_stats->obj.arrays.find(f->index) : nullptr;
        const repl::FastArrayState* shown = f && shown_stats ? shown_stats->obj.arrays.find(f->index) : nullptr;
        if (mine) {
            for (const repl::FastArrayItem& it : mine->items()) {
                const repl::FastArrayItem* there = shown ? shown->find(it.id) : nullptr;
                ++total;
                if (there && there->values == it.values) ++held;
                else ++missed;
            }
        }
        for (size_t i = from; i < b.got.size(); ++i) {
            const fake::Delivered& d = b.got[i];
            if (d.channel != pch || d.close) continue;
            const auto got = sent_array(d, pawn_copy, "UAoCStatsComponent", name);
            if (!got) continue;
            for (const repl::FastArrayItem& it : got->items) {
                const repl::FastArrayItem* owner = mine ? mine->find(it.id) : nullptr;
                wrong += !(owner && owner->values == it.values);
            }
        }
    }
    CHECK(total > 0 && held == total && missed == 0 && wrong == 0, "the pawn the watchers hold has the player's EveryoneProxy stats after the level (the stat items the chains carry are the player's values)");
    const auto calls = level_up_calls(b, pch, from);
    CHECK(calls.size() == 1 && calls[0].first == content::kGuidAdventuring && calls[0].second == 2,
          "the level-up effect is sent to the watcher once: BroadcastPlayLevelUpEffects(the Adventuring category, 2)");
    const auto own_calls = level_up_calls(a, a.pawn_ch, from_own);
    CHECK(own_calls.size() == 1 && own_calls[0].second == 2, "...and the owner's own client has its one call, not a second from the watchers' path");
}
AOC_REGISTER_TEST(test_others_see_a_players_level_up);

// ---- a second and a third watcher --------------------------------------------------------------------------------------------------------------------

static void test_a_second_and_a_third_watcher_see_the_same_changes() {
    std::printf("a second and a third watcher see a player's gear change and its level-up, each on its own channel\n");
    Harness a(cyb_record());
    a.play();
    Players all;
    auto c = join_world(a, all, gear_record(3, "kira"));
    auto b = join_world(a, all, other_record(other_id(2), "ayla", 4));
    a.run(milliseconds(300));
    const world::WorldActor* pawn = shown_pawn(a);
    const uint32_t pch_b = pawn && b->s->actor_view() ? b->s->actor_view()->channel_of(pawn->id) : 0;
    const uint32_t pch_c = pawn && c->s->actor_view() ? c->s->actor_view()->channel_of(pawn->id) : 0;
    CHECK(pawn != nullptr && pch_b != 0 && pch_c != 0, "(both watchers hold the player's pawn, each on a channel of its own connection)");
    if (!pawn || pch_b == 0 || pch_c == 0) return;
    const repl::Actor pawn_copy = pawn->actor;
    const auto chest = a.s->inventory().at(Worn, 2);
    CHECK(chest != nullptr, "(the chest is worn)");
    if (!chest) return;
    const size_t from_b = b->got.size(), from_c = c->got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, chest->instance_guid), true)});
    a.run(milliseconds(250));
    CHECK(arrays_on(*b, pch_b, pawn_copy, from_b, "UPlayerInfo", "HeldItems").size() == 1, "the first watcher is sent the gear change once");
    CHECK(arrays_on(*c, pch_c, pawn_copy, from_c, "UPlayerInfo", "HeldItems").size() == 1, "the second watcher too");
    const size_t from_b2 = b->got.size(), from_c2 = c->got.size();
    a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    a.run(milliseconds(300));
    const auto call_b = level_up_calls(*b, pch_b, from_b2), call_c = level_up_calls(*c, pch_c, from_c2);
    CHECK(call_b.size() == 1 && call_b[0].second == 2, "the first watcher is sent the level-up effect once");
    CHECK(call_c.size() == 1 && call_c[0].second == 2, "the second watcher too");
}
AOC_REGISTER_TEST(test_a_second_and_a_third_watcher_see_the_same_changes);

// ---- a watcher who comes into range after the changes -----------------------------------------------------------------------------------------------

static void test_a_watcher_who_comes_into_range_sees_the_current_gear_weapons_and_level() {
    std::printf("a player who comes into range after a gear change, a swap and a level is shown the character as it is now, and no old level-up effect\n");
    Scene sc(30000);   // the watcher stands 300 m east of where the player starts
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) == 0, "300 m away the watcher holds nothing of the player");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    // the changes, out of the watcher's sight: the chest and the ranged book off, a level, the ranged set in hand
    const auto chest = a.s->inventory().at(Worn, 2);
    const auto book = a.s->inventory().at(Worn, 17);
    CHECK(chest != nullptr && book != nullptr, "(the chest and the ranged book are worn)");
    if (!chest || !book) return;
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, chest->instance_guid), true)});
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 17, book->instance_guid), true)});
    a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    a.client_sends({a.client.actor(a.pawn_ch, ranged_rpc(true), true)});
    a.run(milliseconds(400));
    CHECK(!a.s->inventory().at(Worn, 2) && !a.s->inventory().at(Worn, 17) && a.s->progress().level == 2 && a.s->using_ranged_weapon(),
          "(the player has changed all of them out of sight)");
    CHECK(sc.channel(pawn_id) == 0, "(the watcher still holds nothing of the player)");
    // it comes into range
    const GoodPosition at = a.s->movement().good();
    const PlayerSession::GmMoveResult moved = b.s->gm_teleport(b.conn, at.x + 50.0, at.y, at.z, b.t, "the test");
    CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "(the watcher is put beside the player)");
    a.run(milliseconds(500));
    const world::WorldActor* pawn2 = shown_pawn(a);
    const uint32_t pch = pawn2 ? sc.channel(pawn2->id) : 0;
    CHECK(pch != 0, "(its view opens the player's pawn)");
    const fake::Delivered* open = pch != 0 ? open_on(b, pch) : nullptr;
    CHECK(open != nullptr, "...with an open");
    if (!pawn2 || !open) return;
    const repl::Actor pawn_copy = pawn2->actor;
    const States st = parse_states(*open, pawn_copy);
    CHECK(number_in(st, id_of_component(*pawn2, "UPlayerInfo"), "Level") == 2, "the open carries the new level (2)");
    const auto held = sent_array(*open, pawn_copy, "UPlayerInfo", "HeldItems");
    CHECK(held && record_in(held_slots(*held), 17) == 0 && record_in(held_slots(*held), 15) == fi::kScepter, "...and the gear it has now (the book's held slot empty, the scepter in place)");
    const auto look = array_value_in(st, id_of_component(*pawn2, "UCharacterAppearanceComponent"), "AppearanceIDs");
    CHECK(look && look->elements.size() == 3, "...the three appearance pieces");
    CHECK(number_in(st, id_of_component(*pawn2, "UPlayerCombatInfo"), "bIsUsingRangedWeapon") == 1, "...and the weapon set in hand (the ranged one)");
    const size_t after_open = static_cast<size_t>(open - b.got.data()) + 1;   // (an open is not a call: read the chains after it)
    CHECK(level_up_calls(b, pch, after_open).empty(), "the level-up effect of the time before it watched is not replayed");
}
AOC_REGISTER_TEST(test_a_watcher_who_comes_into_range_sees_the_current_gear_weapons_and_level);

// ---- a relog -----------------------------------------------------------------------------------------------------------------------------------------

static void test_a_relogged_player_is_seen_with_its_gear_and_level_and_the_changes_go_on() {
    std::printf("a player who logs out and back in is seen again with its level and gear, and its next gear change is sent too\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const auto chest = a.s->inventory().at(Worn, 2);
    CHECK(chest != nullptr, "(the chest is worn)");
    if (!chest) return;
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, chest->instance_guid), true)});
    a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    a.run(milliseconds(300));
    CHECK(a.s->progress().level == 2 && !a.s->inventory().at(Worn, 2), "(the player's level and gear changed before the relog)");
    const GoodPosition saved = a.s->last_position();
    const size_t from = b.got.size();
    a.s->finish("the test");
    a.run(milliseconds(250));
    CHECK(shown_pawn(a) == nullptr, "(the pawn left the registry with the session)");
    const auto closes = closes_on(b, pch, from);
    CHECK(closes.size() == 1 && closes[0] == 0, "the watcher's channel of the pawn was closed with Destroyed");
    // it logs in again, at the place it was saved at, with the level its record has now
    svc::Character again = cyb_record();
    again.level = 2;
    again.has_position = true;
    again.position.x = saved.x;
    again.position.y = saved.y;
    again.position.z = saved.z;
    auto a2 = join_world(a, sc.all, again);
    a.run(milliseconds(600));
    const world::WorldActor* pawn2 = shown_pawn(*a2);
    CHECK(pawn2 != nullptr, "(the player is in the world again)");
    if (!pawn2) return;
    const uint32_t pch2 = sc.channel(pawn2->id);
    CHECK(pch2 != 0, "the watcher is sent the returned player's pawn");
    const fake::Delivered* open = pch2 != 0 ? open_on(b, pch2, from) : nullptr;
    CHECK(open != nullptr, "...with an open");
    if (pch2 == 0 || !open) return;
    const repl::Actor pawn_copy = pawn2->actor;
    const States st = parse_states(*open, pawn_copy);
    CHECK(number_in(st, id_of_component(*pawn2, "UPlayerInfo"), "Level") == 2, "the open carries its record's level (2)");
    const auto held = sent_array(*open, pawn_copy, "UPlayerInfo", "HeldItems");
    CHECK(held && record_in(held_slots(*held), 17) == fi::kBook, "...and its kit (the ranged book in its held slot)");
    // a change after the relog reaches the watcher again (the new session keeps its own shown pawn in line)
    const auto chest2 = a2->s->inventory().at(Worn, 2);
    CHECK(chest2 != nullptr, "(the returned player's chest is worn)");
    if (!chest2) return;
    const size_t from2 = b.got.size();
    a2->client_sends({a2->client.actor(a2->pawn_ch, item_used(4, 2, chest2->instance_guid), true)});
    a2->run(milliseconds(250));
    CHECK(arrays_on(b, pch2, pawn_copy, from2, "UPlayerInfo", "HeldItems").size() == 1, "a gear change after the relog is sent once more");
}
AOC_REGISTER_TEST(test_a_relogged_player_is_seen_with_its_gear_and_level_and_the_changes_go_on);

// ---- the same item twice -----------------------------------------------------------------------------------------------------------------------------

static void test_the_same_item_twice_is_shown_once() {
    std::printf("the same item taken off or put on twice is shown once: the second request changes nothing and sends nothing\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const auto chest = a.s->inventory().at(Worn, 2);
    CHECK(chest != nullptr, "(the chest is worn)");
    if (!chest) return;
    const std::string guid = chest->instance_guid;
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, guid), true)});
    a.run(milliseconds(200));
    CHECK(arrays_on(b, pch, pawn_copy, from, "UPlayerInfo", "HeldItems").size() == 1, "(the chest taken off: one update)");
    const size_t applied = a.s->item_requests_applied();
    const size_t from2 = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, guid), true)});
    a.run(milliseconds(200));
    CHECK(a.s->item_requests_applied() == applied, "(the same request again changes nothing)");
    CHECK(arrays_on(b, pch, pawn_copy, from2, "UPlayerInfo", "HeldItems").empty(), "...and the watcher is sent nothing");
    // and the same equip twice
    const int bag_slot = bag_slot_of(*a.s, guid);
    CHECK(bag_slot >= 0, "(the chest is in the bag)");
    if (bag_slot < 0) return;
    const size_t from3 = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(3, bag_slot, guid), true)});
    a.run(milliseconds(200));
    CHECK(arrays_on(b, pch, pawn_copy, from3, "UPlayerInfo", "HeldItems").size() == 1, "(the chest put on: one update)");
    const size_t applied2 = a.s->item_requests_applied();
    const size_t from4 = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(3, bag_slot, guid), true)});
    a.run(milliseconds(200));
    CHECK(a.s->item_requests_applied() == applied2 && arrays_on(b, pch, pawn_copy, from4, "UPlayerInfo", "HeldItems").empty(),
          "the same equip again changes nothing and sends nothing");
}
AOC_REGISTER_TEST(test_the_same_item_twice_is_shown_once);

// ---- two players side by side --------------------------------------------------------------------------------------------------------------------------

static void test_two_players_change_their_gear_side_by_side() {
    std::printf("two players who change their gear side by side are each seen by the watcher, on their own pawn's channel, and by each other\n");
    Harness a(cyb_record());
    a.play();
    Players all;
    auto c = join_world(a, all, gear_record(3, "kira"));
    auto b = join_world(a, all, other_record(other_id(2), "ayla", 4));
    a.run(milliseconds(300));
    const world::WorldActor* pa = shown_pawn(a);
    const world::WorldActor* pc = shown_pawn(*c);
    CHECK(pa != nullptr && pc != nullptr, "(the two players' pawns are in the world)");
    if (!pa || !pc) return;
    const uint32_t pa_b = b->s->actor_view()->channel_of(pa->id), pc_b = b->s->actor_view()->channel_of(pc->id);
    const uint32_t pa_c = c->s->actor_view()->channel_of(pa->id), pc_a = a.s->actor_view()->channel_of(pc->id);
    CHECK(pa_b != 0 && pc_b != 0 && pa_b != pc_b, "the watcher holds both pawns, each on a channel of its own");
    CHECK(pa_c != 0 && pc_a != 0, "the two players hold each other's pawn");
    if (!pa_b || !pc_b || !pa_c || !pc_a) return;
    const repl::Actor pa_copy = pa->actor, pc_copy = pc->actor;
    const auto chest_a = a.s->inventory().at(Worn, 2), chest_c = c->s->inventory().at(Worn, 2);
    CHECK(chest_a != nullptr && chest_c != nullptr, "(both wear a chest)");
    if (!chest_a || !chest_c) return;
    const size_t from_b = b->got.size(), from_a = a.got.size(), from_c = c->got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, chest_a->instance_guid), true)});
    c->client_sends({c->client.actor(c->pawn_ch, item_used(4, 2, chest_c->instance_guid), true)});
    a.run(milliseconds(300));
    CHECK(arrays_on(*b, pa_b, pa_copy, from_b, "UPlayerInfo", "HeldItems").size() == 1, "the watcher is sent the first player's change once");
    CHECK(arrays_on(*b, pc_b, pc_copy, from_b, "UPlayerInfo", "HeldItems").size() == 1, "and the second player's too, on its own channel");
    CHECK(arrays_on(a, pc_a, pc_copy, from_a, "UPlayerInfo", "HeldItems").size() == 1, "the first player is sent the second's change");
    CHECK(arrays_on(*c, pa_c, pa_copy, from_c, "UPlayerInfo", "HeldItems").size() == 1, "the second player is sent the first's");
}
AOC_REGISTER_TEST(test_two_players_change_their_gear_side_by_side);

// ---- a change while the player rides --------------------------------------------------------------------------------------------------------------------

static void test_a_players_gear_change_while_riding_is_shown_on_the_seated_pawn() {
    std::printf("a rider's gear change is shown on its seated pawn's channel and does not touch the attachment\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the player rides)");
    if (!a.s->mounted()) return;
    const world::WorldActor* pawn = shown_pawn(a);
    const world::WorldActor* mount = a.mount();
    CHECK(pawn != nullptr && mount != nullptr, "(the watcher holds the player's pawn and its mount)");
    if (!pawn || !mount) return;
    const uint32_t pch = sc.channel(pawn->id);
    CHECK(pch != 0, "(the watcher holds the seated pawn)");
    const emit::FIntrepidNetworkGUID mount_guid = mount->actor.guid;
    const repl::Actor pawn_copy = pawn->actor;
    const auto book = a.s->inventory().at(Worn, 17);
    CHECK(book != nullptr, "(the ranged book is worn)");
    if (!book) return;
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 17, book->instance_guid), true)});
    a.run(milliseconds(250));
    CHECK(!a.s->inventory().at(Worn, 17) && a.s->mounted(), "(the book is off and the player still rides)");
    const auto chains = arrays_on(b, pch, pawn_copy, from, "UPlayerInfo", "HeldItems");
    CHECK(chains.size() == 1 && record_in(held_slots(chains[0].second), 17) == 0, "the watcher is sent the gear change once, on the seated pawn's channel");
    if (chains.size() == 1) {
        CHECK(value_in(parse_states(*chains[0].first, pawn_copy), 0, "AttachmentReplication.AttachParent") == nullptr, "...and the update carries nothing of the attachment: it stays seated");
    }
    const world::WorldActor* now = shown_pawn(a);
    CHECK(now != nullptr && same_object(actor_object(now->actor, "AttachmentReplication.AttachParent"), mount_guid), "the pawn the watchers hold is still attached to the mount");
}
AOC_REGISTER_TEST(test_a_players_gear_change_while_riding_is_shown_on_the_seated_pawn);

// ---- the first open, unchanged ---------------------------------------------------------------------------------------------------------------------------

static void test_the_first_open_still_carries_the_level_gear_and_look() {
    std::printf("the first open still carries the seven components, the level, the 23 held items and the appearance list: the earlier milestone's values stay right\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const fake::Delivered* open = open_on(b, pch);
    CHECK(open != nullptr, "(the watcher's client has its open)");
    if (!open) return;
    const States st = parse_states(*open, pawn_copy);
    const uint64_t base_id = id_of_component(*pawn, "UPlayerInfo");
    const uint64_t look_id = id_of_component(*pawn, "UCharacterAppearanceComponent");
    const uint64_t stats_id = id_of_component(*pawn, "UAoCStatsComponent");
    const uint64_t combat_id = id_of_component(*pawn, "UPlayerCombatInfo");
    CHECK(pawn_copy.subobjects.size() == 7 && base_id != 0 && look_id != 0 && stats_id != 0 && combat_id != 0, "it has the seven components that show the character (the guild's name included)");
    const auto level = number_in(st, base_id, "Level");
    CHECK(!level || *level == 1, "the level is the class default (1), or not on the wire at all (a level-1 character's pawn does not send it)");
    const auto held = sent_array(*open, pawn_copy, "UPlayerInfo", "HeldItems");
    CHECK(held && held->items.size() == 23 && record_in(held_slots(*held), 15) == fi::kScepter && record_in(held_slots(*held), 17) == fi::kBook,
          "the 23 held items, with the scepter and the book in their slots");
    const auto look = array_value_in(st, look_id, "AppearanceIDs");
    CHECK(look && look->elements.size() == 4, "the four appearance pieces");
    CHECK(sent_array(*open, pawn_copy, "UAoCStatsComponent", "StatRepInt32EveryoneProxy").has_value() &&
              sent_array(*open, pawn_copy, "UAoCStatsComponent", "StatRepFloatArrayEveryoneProxy").has_value(),
          "the two EveryoneProxy stat arrays");
}
AOC_REGISTER_TEST(test_the_first_open_still_carries_the_level_gear_and_look);

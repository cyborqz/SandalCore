// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_emotes_seen_tests.inc: the emotes the other players see (step 5 of the social track)
//
//  The pawn the other players are shown (an actor of kind Player in the world's registry) carries an AbilityComponent with the records of the emote abilities and, while the player's own pawn
//  holds an emote's ability item (the Rest's, Emote_Wave's ...), the same item without what only the owner can resolve.  Every player whose view holds the pawn, within 150 m, gets it as the
//  update of any other actor.  Only emotes are shown; the Sprint, the Dodge and the combat abilities are not (yet).
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================
#include "content/emote_commands.h"

/// The client is ready to phase in: the player's pawn is put into the world's registry for the other players within a tick or two.
static void emote_phase_in(Run& r) {
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, false);
}

/// The Player actor of the registry that is this session's pawn as the other players are shown it (null before the phase-in).
static const world::WorldActor* shown_pawn_of(Run& r) {
    const uint64_t id = static_cast<uint64_t>(r.h.s->bootstrap().setup().player_id);
    const world::WorldActor* found = nullptr;
    r.h.fx.world->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Player && a.owner == id) found = &a;
    });
    return found;
}

/// One element of the ReplicatedActiveAbilities array of the shown pawn.
struct ShownItem {
    int32_t     element = 0;
    int64_t     ability = 0, instance = 0;
    uint64_t    montage = 0, instigator = 0;
    float       position = 0, play_rate = 0;
    bool        jump = false, predicted = true;
    int32_t     counter = 0;
    size_t      targets = 0;
    std::string group;
};

static const repl::Subobject* shown_ability_component(const world::WorldActor& a) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UAoCAbilityComponent") return &s;
    }
    return nullptr;
}

static std::vector<ShownItem> shown_items(const world::WorldActor& a) {
    std::vector<ShownItem> out;
    const repl::Subobject* s = shown_ability_component(a);
    if (!s) return out;
    repl::FastArrays arrays = s->obj.arrays;   // (FastArrays::get creates an array on first use: a copy, to read it)
    const repl::FastArrayState* arr = arrays.get(*s->obj.schema, "ReplicatedActiveAbilities");
    if (!arr) return out;
    for (const repl::FastArrayItem& it : arr->items()) {
        ShownItem si;
        si.element = it.id;
        const auto val = [&](uint16_t handle) -> const repl::Value* {
            const auto f = it.values.values.find(handle);
            return f == it.values.values.end() ? nullptr : &f->second;
        };
        if (const repl::Value* v = val(1)) si.ability = v->get_if<int64_t>() ? *v->get_if<int64_t>() : 0;
        if (const repl::Value* v = val(2)) si.instance = v->get_if<int64_t>() ? *v->get_if<int64_t>() : 0;
        if (const repl::Value* v = val(3)) si.targets = v->get_if<repl::ArrayValue>() ? v->get_if<repl::ArrayValue>()->elements.size() : 0;
        if (const repl::Value* v = val(9)) si.predicted = v->get_if<bool>() && *v->get_if<bool>();
        if (const repl::Value* v = val(10)) si.group = v->get_if<repl::NameValue>() ? v->get_if<repl::NameValue>()->text : std::string();
        if (const repl::Value* v = val(11)) si.montage = v->get_if<emit::FIntrepidNetworkGUID>() ? v->get_if<emit::FIntrepidNetworkGUID>()->ObjectId : 0;
        if (const repl::Value* v = val(12)) si.play_rate = v->get_if<float>() ? *v->get_if<float>() : 0.0f;
        if (const repl::Value* v = val(13)) si.position = v->get_if<float>() ? *v->get_if<float>() : 0.0f;
        if (const repl::Value* v = val(14)) si.jump = v->get_if<bool>() && *v->get_if<bool>();
        if (const repl::Value* v = val(15)) si.counter = v->get_if<int64_t>() ? static_cast<int32_t>(*v->get_if<int64_t>()) : 0;
        if (const repl::Value* v = val(17)) si.instigator = v->get_if<emit::FIntrepidNetworkGUID>() ? v->get_if<emit::FIntrepidNetworkGUID>()->ObjectId : 0;
        out.push_back(si);
    }
    return out;
}

/// The ability records the shown pawn's AbilityComponent asks the other players' clients to load.
static std::vector<int64_t> shown_records(const world::WorldActor& a) {
    std::vector<int64_t> out;
    const repl::Subobject* s = shown_ability_component(a);
    const repl::Value* v = s ? s->obj.state.get(*s->obj.schema, "ReplicatedRecordsToLoad") : nullptr;
    const repl::ArrayValue* list = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!list) return out;
    for (const repl::ElementState& e : list->elements) {
        const auto f = e.values.find(1);
        if (f != e.values.end() && f->second.get_if<int64_t>()) out.push_back(*f->second.get_if<int64_t>());
    }
    return out;
}

static void test_the_pawn_other_players_are_shown_holds_the_emote_records() {
    std::printf("the pawn the other players are shown has an AbilityComponent with the emote records, no ability item yet and nothing of the owner's lists\n");
    Run r;
    emote_phase_in(r);
    const world::WorldActor* pawn = shown_pawn_of(r);
    CHECK(pawn != nullptr, "the player's pawn is in the registry for the other players");
    if (!pawn) return;
    const repl::Subobject* comp = shown_ability_component(*pawn);
    CHECK(comp != nullptr, "it has an AbilityComponent");
    if (!comp) return;
    CHECK(shown_items(*pawn).empty() && pawn->ability_exports.empty(), "...with no active ability and no export");
    const std::vector<int64_t> records = shown_records(*pawn);
    size_t emotes_known = 0;
    std::set<int64_t> seen_abilities;
    for (const auto& e : content::kEmoteCommands) {
        if (seen_abilities.insert(e.ability).second && std::find(r.h.s->known_abilities().begin(), r.h.s->known_abilities().end(), e.ability) != r.h.s->known_abilities().end()) ++emotes_known;
    }
    bool only_emotes = true;
    for (const int64_t g : records) only_emotes = only_emotes && content::is_emote_ability(g);
    CHECKF(records.size() == emotes_known && emotes_known >= 33, "it asks for the records of the %zu emote abilities the character knows (%zu listed)", emotes_known, records.size());
    CHECK(only_emotes && std::find(records.begin(), records.end(), static_cast<int64_t>(107140329395832ll)) != records.end() &&
              std::find(records.begin(), records.end(), content::kGuidRest) != records.end(),
          "...and only those: Emote_Wave and General_Rest among them, no record of the class's abilities");
    CHECK(comp->obj.state.get(*comp->obj.schema, "KnownAbilities") == nullptr, "it has no KnownAbilities (the list is the owner's)");
    repl::FastArrays arrays = comp->obj.arrays;
    const repl::FastArrayState* cooldowns = arrays.get(*comp->obj.schema, "Cooldowns");
    CHECK(cooldowns == nullptr || cooldowns->items().empty(), "and no cooldown (those are the owner's too)");
}

AOC_REGISTER_TEST(test_the_pawn_other_players_are_shown_holds_the_emote_records);

static void test_a_sit_is_shown_to_the_other_players() {
    std::printf("/sit is shown to the other players: the Rest's item with its montage, then its end section, then a new item\n");
    Run r;
    emote_phase_in(r);
    const world::WorldActor* pawn = shown_pawn_of(r);
    CHECK(pawn != nullptr, "the player's pawn is in the registry for the other players");
    if (!pawn) return;
    const uint32_t version = pawn->version;
    say_emote(r, "sit");
    r.play(0.3, false);
    std::vector<ShownItem> items = shown_items(*pawn);
    const auto rest_montage = bootstrap::montage_for(bootstrap::kRestMontagePath);
    CHECK(rest_montage.has_value(), "(the Rest montage's id is known)");
    CHECK(items.size() == 1 && items[0].ability == kRest && rest_montage && items[0].montage == rest_montage->guid.ObjectId, "sit: one item, the Rest's, with the Rest montage");
    CHECK(pawn->version > version && pawn->ability_exports.size() == 1, "...the pawn counts the change and carries the montage's export, which the update of any viewer that has not got it takes along");
    bool owners = false;
    for (const auto& [eid, a] : r.m.abilities) owners = owners || (a.ability == kRest && !items.empty() && a.instance == items[0].instance && a.animation_counter == items[0].counter);
    CHECK(owners, "it is the owner's item: the same ServerInstanceId and AnimationCounter");
    CHECK(items.size() == 1 && items[0].targets == 0 && !items[0].jump && items[0].position == 0.0f && items[0].group == "DefaultGroup" && items[0].instigator == pawn->actor.guid.ObjectId,
          "without the caster's collision cylinder as the target (the other players have no such object), no jump, MontageGroup DefaultGroup, the pawn itself as the instigator");
    CHECK(items.size() == 1 && !items[0].predicted, "it says the client did not predict it (bBranchPredictedOnAutonomous 0), as the game's own item of an emote does");
    const int32_t element = items.empty() ? 0 : items[0].element;

    say_emote(r, "sit");   // stand up
    r.play(0.1, false);
    items = shown_items(*pawn);
    CHECK(items.size() == 1 && items[0].element == element && items[0].jump && items[0].position == 5.82606792f && items[0].counter == 1,
          "a second sit: the same item changes in place, the montage jumps to its end section (SitEnd), AnimationCounter 1");

    say_emote(r, "sit");   // sit down again
    r.play(0.3, false);
    items = shown_items(*pawn);
    CHECK(items.size() == 1 && items[0].element != element && !items[0].jump && items[0].counter == 3 && items[0].montage == (rest_montage ? rest_montage->guid.ObjectId : 0),
          "a third sit: the ended item is replaced by a new one (AnimationCounter 3)");
    CHECK(pawn->ability_exports.size() == 1, "(one export, not two)");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the owner's own view is as before: abilities on, every chain reads)");
}

AOC_REGISTER_TEST(test_a_sit_is_shown_to_the_other_players);

static void test_a_wave_is_shown_to_the_other_players() {
    std::printf("/wave is shown to the other players: Emote_Wave's item, replaced by the next one\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    Run r(0, true);
    emote_phase_in(r);
    const world::WorldActor* pawn = shown_pawn_of(r);
    CHECK(pawn != nullptr, "the player's pawn is in the registry for the other players");
    if (!pawn) return;
    std::string wave_path;
    const uint64_t wave_id = emote_montage_id(r, kEmoteWave, &wave_path);
    CHECK(wave_id != 0 && !wave_path.empty(), "(the tables hold Emote_Wave's montage)");
    say_emote(r, "wave");
    r.play(0.6, false);
    std::vector<ShownItem> items = shown_items(*pawn);
    CHECK(items.size() == 1 && items[0].ability == kEmoteWave, "wave: one item, Emote_Wave's");
    bool owners = false;
    for (const auto& [eid, a] : r.m.abilities) owners = owners || (a.ability == kEmoteWave && !items.empty() && a.instance == items[0].instance && a.animation_counter == items[0].counter);
    CHECK(owners, "it is the owner's item: the same ServerInstanceId and AnimationCounter");
    CHECK(!items.empty() && items[0].montage == wave_id && items[0].instigator == pawn->actor.guid.ObjectId,
          "the item names the Wave02 montage's id, the pawn itself as the instigator");
    CHECK(pawn->ability_exports.size() == 1, "...and the pawn carries the montage's export");
    bool chain_ok = false;
    for (const repl::ExportChain& chain : pawn->ability_exports) {
        if (chain.size() != 2) continue;
        const size_t dot = wave_path.rfind('.');
        const std::string object_name = dot == std::string::npos ? std::string() : wave_path.substr(dot + 1);
        const std::string package_path = dot == std::string::npos ? std::string() : wave_path.substr(0, dot);
        chain_ok = chain[0].guid.ObjectId == wave_id && chain[0].name == object_name && chain[1].name == package_path;
    }
    CHECK(chain_ok, "the export names the montage's path with that id (the object and its package)");
    CHECK(!items.empty() && !items[0].predicted && items[0].group == "DefaultGroup", "it says the client did not predict it (bBranchPredictedOnAutonomous 0), MontageGroup DefaultGroup, as the game's own item of an emote does");
    const int64_t first = items.empty() ? 0 : items[0].instance;
    r.play(1.4, false);   // the cooldown is over
    say_emote(r, "hi");
    r.play(0.6, false);
    items = shown_items(*pawn);
    CHECK(items.size() == 1 && items[0].ability == kEmoteWave && items[0].instance != first, "a second wave 2 s later replaces the first: one item, a new instance");
    // another emote replaces it too
    r.play(1.0, false);
    say_emote(r, "bow");
    r.play(0.6, false);
    items = shown_items(*pawn);
    const int64_t bow = [&] {
        const auto* e = content::find_emote_command("bow");
        return e ? e->ability : 0;
    }();
    CHECK(items.size() == 1 && items[0].ability == bow && bow != 0, "a bow after it: the wave's item is replaced by the Bow's");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine)");
}

AOC_REGISTER_TEST(test_a_wave_is_shown_to_the_other_players);

static void test_only_emotes_are_shown_to_the_other_players() {
    std::printf("only the emotes are shown to the other players: a Sprint and a Dodge change nothing on the pawn they see\n");
    Run r;
    emote_phase_in(r);
    const world::WorldActor* pawn = shown_pawn_of(r);
    CHECK(pawn != nullptr, "the player's pawn is in the registry for the other players");
    if (!pawn) return;
    const uint32_t version = pawn->version;
    const int32_t sprint = r.trigger(kSprint);
    r.play(0.5, true);
    CHECK(r.m.has_ability(kSprint), "(the owner runs a Sprint: its item is on the owner's pawn)");
    r.cancel(sprint);
    r.play(0.3, true);
    r.trigger(kDodge);
    r.play(0.6, true);
    CHECK(r.m.has_ability(kDodge), "(and a Dodge)");
    CHECK(shown_items(*pawn).empty() && pawn->ability_exports.empty(), "the pawn the others see holds no ability item and no export");
    // an emote in between is shown, and stays shown through the Sprint that follows
    say_emote(r, "sit");
    r.play(0.3, false);
    CHECK(shown_items(*pawn).size() == 1, "(a sit is shown)");
    const int32_t again = r.trigger(kSprint);
    r.play(0.3, true);
    r.cancel(again);
    r.play(0.3, true);
    const std::vector<ShownItem> items = shown_items(*pawn);
    CHECK(items.size() == 1 && items[0].ability == kRest, "a Sprint ends the Rest on the owner (its end section), and the others are shown that change, not the Sprint's item");
    (void)version;
}

AOC_REGISTER_TEST(test_only_emotes_are_shown_to_the_other_players);

static void test_an_emote_before_the_pawn_is_shown_reaches_it_when_it_is() {
    std::printf("an emote made before the player's pawn is in the registry is shown when it gets there\n");
    Run r;
    r.play(0.5, false);
    CHECK(shown_pawn_of(r) == nullptr, "(the client is not ready to phase in yet: the registry holds no pawn for the player)");
    say_emote(r, "sit");
    r.play(0.3, false);
    CHECK(r.m.has_ability(kRest), "(the owner sits)");
    emote_phase_in(r);
    const world::WorldActor* pawn = shown_pawn_of(r);
    CHECK(pawn != nullptr, "the pawn is in the registry now");
    if (!pawn) return;
    const std::vector<ShownItem> items = shown_items(*pawn);
    CHECK(items.size() == 1 && items[0].ability == kRest, "...and it shows the Rest the owner sat down in");
}

AOC_REGISTER_TEST(test_an_emote_before_the_pawn_is_shown_reaches_it_when_it_is);

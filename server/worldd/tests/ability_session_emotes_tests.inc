// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_emotes_tests.inc: /sit, /wave and the other emote commands through a session (step 5 of the social track)
//
//  A client sends ServerPlayEmote with the command word (no slash).  /sit is the Rest: the first one sits the character down (the Rest's ability item with its montage, the Resting effect two
//  seconds on), the second one ends the Rest and plays the montage's SitEnd section.  Every other word plays the ability of the game's emote list through the generic ability runtime, with
//  the record's cooldown and cost.  A word that is not on the list is refused and logged; a /sit in combat is refused and logged; a refusal sends the client nothing (it began no instance of
//  its own: no ClientEndAbility).
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================
#include "content/emote_commands.h"
#include "quest/quest_log.h"

static constexpr int64_t kEmoteWave = 107140329395832ll;   // Emote_Wave: /wave and /hi
static constexpr int64_t kEmoteDance = 6064629698452980380ll;   // Emote_Dance: /dance

/// The id the server gives the first montage of `ability`, computed from the table's path with the recipe (a second way to the server's own).
static uint64_t emote_montage_id(const Run& r, int64_t ability, std::string* path = nullptr) {
    const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
    const abilities::AbilityDef* def = cat ? cat->ability(ability) : nullptr;
    if (!def || def->montages.empty()) return 0;
    if (path) *path = def->montages.front().soft_path;
    return quest::city_hash64("AnimMontage " + def->montages.front().soft_path) | 1;
}

/// The id the server gives a montage: the recorded one the server's table holds (the Dance04 montage), else the computed recipe.
static uint64_t expected_montage_id(const std::string& path) {
    if (path.find("All_EMO_M_Dance04_Montage") != std::string::npos) return 80184083730092395ull;
    return quest::city_hash64("AnimMontage " + path) | 1;
}

/// The ability items one delivered chain carries: (ability guid, montage ObjectId).
static std::vector<std::pair<int64_t, uint64_t>> chain_ability_items(const Run& r, const fake::Delivered& d) {
    std::vector<std::pair<int64_t, uint64_t>> out;
    const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!views) return out;
    const uint64_t ability_object = r.h.s->bootstrap().setup().guids.ability.ObjectId;
    for (const repl::BlockView& v : *views) {
        if (v.header.is_actor || v.header.object.ObjectId != ability_object) continue;
        size_t p = v.payload_pos;
        const auto fields = repl::read_fast_array_fields(class_named("UAoCAbilityComponent"), d.content.data(), v.payload_pos + v.payload_bits, p, stat_options());
        if (!fields) continue;
        for (const repl::FastArrayField& f : *fields) {
            if (std::string(class_named("UAoCAbilityComponent").field(f.field)->name) != "ReplicatedActiveAbilities") continue;
            for (const auto& c : f.delta.changed) {
                const auto val = [&](uint16_t handle) -> const repl::Value* {
                    const auto it = c.values.values.find(handle);
                    return it == c.values.values.end() ? nullptr : &it->second;
                };
                int64_t ability = 0;
                uint64_t montage = 0;
                if (const repl::Value* x = val(1)) ability = x->get_if<int64_t>() ? *x->get_if<int64_t>() : 0;
                if (const repl::Value* x = val(11)) montage = x->get_if<emit::FIntrepidNetworkGUID>() ? x->get_if<emit::FIntrepidNetworkGUID>()->ObjectId : 0;
                out.emplace_back(ability, montage);
            }
        }
    }
    return out;
}

/// A client's ServerPlayEmote(EmoteName): a call of the PlayerController, `word` as the client sends it (no slash).
static BunchWriter emote_bunch(const std::string& word) {
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

/// The client types an emote command: the call goes to the server in a datagram of its own, and what the server answers is absorbed.
static void say_emote(Run& r, const std::string& word) {
    r.h.client_sends({r.h.client.actor(r.h.pc_ch, emote_bunch(word), /*reliable=*/true)});
    r.h.flush();
    r.absorb();
}

/// The text the server logged so far.
static std::string emote_log() {
    spdlog::default_logger()->flush();
    return g_log.str();
}

static bool text_has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

/// The ServerInstanceIds of the ability items the client holds for `ability`.
static size_t items_of(const Run& r, int64_t ability) {
    size_t n = 0;
    for (const auto& [id, a] : r.m.abilities) n += a.ability == ability ? 1 : 0;
    return n;
}

static bool cooldown_of(const Run& r, int64_t ability) {
    for (const auto& [id, guid] : r.m.cooldowns)
        if (guid == ability) return true;
    return false;
}

static void test_sit_sits_down_and_a_second_sit_stands_up() {
    std::printf("/sit: the character sits down, a second /sit ends the Rest and plays its end section\n");
    Run r;
    r.play(0.5, false);
    const size_t chains = r.m.chains;
    say_emote(r, "sit");
    r.play(0.3, false);
    CHECK(r.m.has_ability(kRest) && r.m.chains > chains, "sit: the Rest's ability item arrives");
    CHECK(r.m.chains_with_exports == 1, "...in one update with the export of the Rest montage (the client has to learn the path)");
    const auto rest_montage = bs::montage_for(bs::kRestMontagePath);
    CHECK(rest_montage.has_value(), "(the Rest montage's recorded id is known)");
    bool item_ok = false;
    for (const auto& [eid, a] : r.m.abilities) item_ok = a.ability == kRest && a.has_montage && rest_montage && a.montage == rest_montage->guid.ObjectId && a.has_target && a.animation_counter == 0 && !a.may_jump;
    CHECK(item_ok, "its item: the Rest montage's recorded id, the caster as target, AnimationCounter 0, no jump");
    CHECK(!r.m.has_effect(kRestEffect) && r.h.s->abilities() && r.h.s->abilities()->resting(), "the Resting effect is not there during the sit-down; the runtime rests");
    r.play(2.2, false);
    CHECK(r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 2.5f && r.m.stats[kManaRegen] == 2.5f, "after the sit-down the Resting effect is on: Health and Mana regeneration are 2.5");
    CHECK(r.h.s->emotes_played() == 1 && r.h.s->emotes_refused() == 0, "(the session counts one emote played, none refused)");

    say_emote(r, "sit");   // a second /sit: the character stands up
    bool jumped = false;
    for (const auto& [eid, a] : r.m.abilities) jumped = a.ability == kRest && a.may_jump && a.montage_position == 5.82606792f && a.animation_counter == 1;
    CHECK(jumped, "a second sit: the montage jumps to its end section (SitEnd), AnimationCounter 1");
    CHECK(r.m.has_effect(kRestEffect) && !r.h.s->abilities()->resting(), "(the effect is still on in that step; the runtime no longer rests)");
    r.play(0.3, false);
    CHECK(!r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 0.6f && r.m.stats[kManaRegen] == 0.6f, "a moment later the effect goes and the regeneration is back to 0.6");
    CHECK(r.m.ends.empty() && r.m.stamina_now() == 100.0f, "(no ClientEndAbility was sent, Stamina is untouched)");
    CHECK(items_of(r, kRest) == 1, "(the client holds one Rest item)");

    // a third sit sits down again: the ended Rest's item is replaced by a new one (AnimationCounter two on, as a second Rest is)
    say_emote(r, "sit");
    r.play(0.3, false);
    bool again = false;
    for (const auto& [eid, a] : r.m.abilities) again = a.ability == kRest && !a.may_jump && a.animation_counter == 3;
    CHECK(again && items_of(r, kRest) == 1 && r.h.s->abilities()->resting(), "a third sit sits down again: one new Rest item (AnimationCounter 3), the runtime rests");
    r.play(2.2, false);
    CHECK(r.m.has_effect(kRestEffect), "...and the Resting effect comes again");
    CHECK(r.h.s->emotes_played() == 3 && r.h.s->emotes_refused() == 0 && !r.h.s->abilities_failed() && r.m.unreadable == 0, "(three sits, none refused, the session is fine)");
}

AOC_REGISTER_TEST(test_sit_sits_down_and_a_second_sit_stands_up);

static void test_walking_away_ends_the_rest() {
    std::printf("/sit: walking away stands the character up and ends the Resting effect; standing still keeps it\n");
    Run r;
    r.play(0.5, false);
    say_emote(r, "sit");
    r.play(2.2, false);
    CHECK(r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 2.5f && r.h.s->abilities()->resting(), "(the character sits and the Resting effect is on: regeneration 2.5)");
    r.play(0.1, true);   // a move: the character walks away
    bool jumped = false;
    for (const auto& [eid, a] : r.m.abilities) jumped = a.ability == kRest && a.may_jump && a.montage_position == 5.82606792f && a.animation_counter == 1;
    CHECK(jumped, "walking away: the montage jumps to its end section (SitEnd), AnimationCounter 1");
    CHECK(!r.h.s->abilities()->resting(), "the runtime no longer rests");
    CHECK(!r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 0.6f && r.m.stats[kManaRegen] == 0.6f, "the Resting effect and its faster regeneration are gone");
    CHECK(items_of(r, kRest) == 1 && r.m.ends.empty(), "(the client holds one Rest item, no ClientEndAbility)");
    // standing still keeps the Rest
    say_emote(r, "sit");
    r.play(2.2, false);
    CHECK(r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 2.5f && r.h.s->abilities()->resting(), "a new sit and standing still: the Resting effect stays on");
    r.play(1.0, false);
    CHECK(r.m.has_effect(kRestEffect) && r.h.s->abilities()->resting(), "...and it stays while the character does not move");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine)");
}

AOC_REGISTER_TEST(test_walking_away_ends_the_rest);

static void test_sit_is_matched_in_any_case() {
    std::printf("/sit: SIT sits down, Sit stands up\n");
    Run r;
    r.play(0.5, false);
    say_emote(r, "SIT");
    r.play(0.3, false);
    CHECK(r.m.has_ability(kRest) && r.h.s->abilities()->resting(), "SIT sits the character down");
    say_emote(r, "sIt");
    r.play(0.1, false);
    bool jumped = false;
    for (const auto& [eid, a] : r.m.abilities) jumped = a.ability == kRest && a.may_jump && a.animation_counter == 1;
    CHECK(jumped && !r.h.s->abilities()->resting(), "sIt stands it up: the montage jumps to its end section");
}

AOC_REGISTER_TEST(test_sit_is_matched_in_any_case);

static void test_a_sit_ends_a_rest_the_client_began() {
    std::printf("/sit: a Rest the client began with its own trigger is ended by /sit as well\n");
    Run r;
    r.play(0.5, false);
    const int32_t id = r.trigger(kRest);
    r.play(0.3, false);
    CHECK(r.m.has_ability(kRest) && r.h.s->abilities()->resting(), "the client's own Rest runs");
    say_emote(r, "sit");
    r.play(0.1, false);
    bool jumped = false;
    for (const auto& [eid, a] : r.m.abilities) jumped = a.ability == kRest && a.may_jump && a.animation_counter == 1;
    CHECK(jumped && !r.h.s->abilities()->resting() && r.m.ends.empty(), "/sit ends it: the montage jumps to its end section, no ClientEndAbility");
    r.cancel(id);   // the client's own cancel of the instance that has ended is taken quietly
    r.play(0.2, false);
    CHECK(!r.h.s->abilities_failed() && r.m.ends.empty(), "(the client's late cancel of the same Rest changes nothing)");
}

AOC_REGISTER_TEST(test_a_sit_ends_a_rest_the_client_began);

static void test_unknown_emote_words_send_nothing_and_are_logged() {
    std::printf("an emote word that is not on the list is refused: nothing is sent, the refusal is logged\n");
    Run r;
    r.play(0.5, false);
    const size_t pc_calls = r.m.pc_calls.size(), effects = r.m.effects.size(), cooldowns = r.m.cooldowns.size();
    g_log.str("");
    const std::vector<std::string> words = {"dancing", "", "wav", "Emote_Wave", "sit ", "/sit", "waves", "General_Rest"};
    for (const std::string& w : words) say_emote(r, w);
    r.play(0.5, false);
    const std::string text = emote_log();
    CHECK(r.m.abilities.empty() && r.m.effects.size() == effects && r.m.cooldowns.size() == cooldowns && r.m.ends.empty() && r.m.pc_calls.size() == pc_calls,
          "nothing is sent: no ability item, no effect, no cooldown, no ClientEndAbility, no call on the PlayerController");
    CHECK(text_has(text, "\"dancing\" is not an emote") && text_has(text, "\"sit \" is not an emote") && text_has(text, "\"General_Rest\" is not an emote"),
          "each refusal is logged with the word");
    CHECK(r.h.s->emotes_refused() == words.size() && r.h.s->emotes_played() == 0 && r.h.s->emotes_dropped() == 0, "(the session counts 8 refused, none played, none dropped)");
    CHECK(r.h.s->abilities() && !r.h.s->abilities()->resting() && !r.h.s->abilities_failed(), "(the runtime is not resting, abilities are on)");
    // a word of 700 characters (the longest one call carries in a packet) is refused, and the log does not repeat all of it
    g_log.str("");
    say_emote(r, std::string(700, 'x'));
    r.play(0.2, false);
    const std::string long_text = emote_log();
    const bool short_log = long_text.size() < 600;
    if (!short_log || !text_has(long_text, "is not an emote")) std::printf("  (the log: %s)\n", long_text.c_str());
    CHECK(text_has(long_text, "is not an emote") && short_log, "a word of 700 characters is refused, and the log line cuts it");
    CHECK(r.m.abilities.empty() && r.m.ends.empty(), "(nothing sent for it either)");
}

AOC_REGISTER_TEST(test_unknown_emote_words_send_nothing_and_are_logged);

static void test_a_flood_of_emotes_is_dropped_at_the_budget() {
    std::printf("a client that sends emotes faster than its budget allows is dropped and counted\n");
    Run r;
    r.play(0.5, false);
    for (int i = 0; i < 14; ++i) say_emote(r, "nope" + std::to_string(i));
    r.play(0.1, false);
    CHECKF(r.h.s->emotes_refused() == 8 && r.h.s->emotes_dropped() == 6, "14 calls in one moment: the burst of 8 is looked at (refused), the other 6 are dropped (%zu refused, %zu dropped)",
           r.h.s->emotes_refused(), r.h.s->emotes_dropped());
    r.play(1.2, false);   // the budget earns a token every quarter second
    say_emote(r, "sit");
    r.play(0.3, false);
    CHECK(r.m.has_ability(kRest) && r.h.s->emotes_played() == 1, "after a second of quiet an emote is taken again");
}

AOC_REGISTER_TEST(test_a_flood_of_emotes_is_dropped_at_the_budget);

static void test_wave_plays_its_ability_with_the_records_cooldown() {
    std::printf("/wave: Emote_Wave plays through its ability record, with the record's one-second cooldown and no cost\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    Run r(0, true);
    r.play(0.5, false);
    CHECK(r.h.s->pipeline() && r.h.s->pipeline()->handles(kEmoteWave) && r.h.s->pipeline()->known(kEmoteWave), "(the generic ability runtime runs Emote_Wave, which every character knows)");
    if (!r.h.s->pipeline()) return;
    const AbilityPipeline& pipe = *r.h.s->pipeline();
    const float mana = r.m.stats[kManaStat], stamina = r.m.stamina_now();
    const size_t started = pipe.casts_started();
    say_emote(r, "wave");
    r.play(0.6, false);
    CHECK(pipe.casts_started() == started + 1 && r.m.has_ability(kEmoteWave), "wave: the cast starts and Emote_Wave's ability item arrives");
    const uint64_t wave_id = emote_montage_id(r, kEmoteWave);
    CHECK(wave_id != 0, "(the tables hold Emote_Wave's montage)");
    bool item_ok = false;
    for (const auto& [eid, a] : r.m.abilities) item_ok = item_ok || (a.ability == kEmoteWave && a.has_montage && a.montage == wave_id);
    CHECK(item_ok, "the item names the Wave02 montage's id");
    CHECK(cooldown_of(r, kEmoteWave), "its Cooldowns item arrives");
    CHECK(r.m.stats[kManaStat] >= mana && r.m.stamina_now() >= stamina, "the emote costs no Mana and no Stamina (its cost is the charge of the cooldown)");
    CHECK(r.m.ends.empty(), "no ClientEndAbility");
    CHECK(r.h.s->emotes_played() == 1, "(the session counts one emote played)");

    // too soon: /hi is the same ability and the same cooldown
    const size_t refused = pipe.refused();
    g_log.str("");
    say_emote(r, "hi");   // 0.6 s after the /wave
    r.play(0.4, false);
    CHECK(pipe.casts_started() == started + 1 && pipe.refused() == refused + 1, "a /hi 0.6 s after the /wave is refused by the cooldown (the same ability)");
    CHECK(r.m.ends.empty() && items_of(r, kEmoteWave) == 1, "...and the client is sent nothing: no ClientEndAbility, no second item");
    CHECK(text_has(emote_log(), "Emote_Wave refused"), "(the refusal is in the log)");
    r.play(1.0, false);   // 2.0 s after the /wave
    say_emote(r, "WAVE");
    r.play(0.6, false);
    CHECK(pipe.casts_started() == started + 2 && items_of(r, kEmoteWave) == 1 && r.m.ends.empty(), "after the cooldown a WAVE plays again; the new item replaces the old one");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine)");
}

AOC_REGISTER_TEST(test_wave_plays_its_ability_with_the_records_cooldown);

static void test_wave_names_the_wave_montage_and_exports_its_path() {
    std::printf("/wave: the item names the Wave02 montage's id and the chain that carries it exports the montage's path with that id\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    Run r(0, true);
    r.play(0.5, false);
    std::string path;
    const uint64_t want = emote_montage_id(r, kEmoteWave, &path);
    CHECK(want != 0 && !path.empty(), "(the tables hold Emote_Wave's montage)");
    if (want == 0) return;
    const size_t dot = path.rfind('.');
    const std::string object_name = dot == std::string::npos ? std::string() : path.substr(dot + 1);
    const std::string package_path = dot == std::string::npos ? std::string() : path.substr(0, dot);
    const size_t before = r.h.got.size();
    say_emote(r, "wave");
    r.play(0.6, false);
    bool item_ok = false;
    for (const auto& [eid, a] : r.m.abilities) item_ok = item_ok || (a.ability == kEmoteWave && a.has_montage && a.montage == want);
    CHECKF(item_ok, "the item names the Wave02 montage's id %llu", static_cast<unsigned long long>(want));
    bool same_chain = false, object_ok = false, package_ok = false;
    for (size_t i = before; i < r.h.got.size(); ++i) {
        const fake::Delivered& d = r.h.got[i];
        bool carries = false;
        for (const auto& [ability, montage] : chain_ability_items(r, d)) carries = carries || (ability == kEmoteWave && montage == want);
        if (!carries) continue;
        const std::vector<ExportRecord> records = export_records(d);
        for (const ExportRecord& rec : records) {
            if (rec.guid.ObjectId != want) continue;
            same_chain = true;
            object_ok = rec.path == object_name;
            for (const ExportRecord& outer : records) {
                if (outer.guid.ObjectId == rec.outer.ObjectId) package_ok = outer.path == package_path;
            }
        }
    }
    CHECK(same_chain, "the chain that carries the item also exports the montage's id");
    CHECK(object_ok && package_ok, "the export names the montage's path: the object and its package");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine)");
}

AOC_REGISTER_TEST(test_wave_names_the_wave_montage_and_exports_its_path);

static void test_dance_picks_one_of_its_four_montages() {
    std::printf("/dance: each cast plays one of the ability's four montages, and all four come up\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    Run r(0, true);
    r.play(0.5, false);
    const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
    const abilities::AbilityDef* def = cat ? cat->ability(kEmoteDance) : nullptr;
    CHECK(def && def->montages.size() == 4, "(the tables hold Emote_Dance with its four montages)");
    if (!def || def->montages.size() != 4) return;
    std::map<uint64_t, std::string> ids;
    for (const content::MontageRow& m : def->montages) ids.emplace(expected_montage_id(m.soft_path), m.soft_path);
    std::set<uint64_t> seen;
    int casts = 0, wrong = 0;
    int64_t last_instance = -1;
    for (int i = 0; i < 24; ++i) {
        say_emote(r, "dance");
        r.play(0.6, false);
        int64_t instance = -1;
        uint64_t montage = 0;
        for (const auto& [eid, a] : r.m.abilities) {
            if (a.ability != kEmoteDance) continue;
            instance = a.instance;
            montage = a.montage;
        }
        if (instance >= 0 && instance != last_instance) {
            ++casts;
            last_instance = instance;
            if (ids.count(montage) == 0) ++wrong;
            else seen.insert(montage);
        }
        r.play(1.0, false);   // the cooldown and the budget
    }
    CHECKF(casts == 24, "24 dances played (%d)", casts);
    CHECK(wrong == 0, "every item named one of the ability's four montages");
    CHECKF(seen.size() == 4, "all four montages came up (%zu of 4)", seen.size());
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine)");
}

AOC_REGISTER_TEST(test_dance_picks_one_of_its_four_montages);

static void test_dance_uses_the_recorded_dance04_chain() {
    std::printf("/dance: the Dance04 montage keeps its recorded object and package ids, the other three keep the computed ids\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    Run r(0, true);
    r.play(0.5, false);
    const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
    const abilities::AbilityDef* def = cat ? cat->ability(kEmoteDance) : nullptr;
    CHECK(def && def->montages.size() == 4, "(the tables hold Emote_Dance with its four montages)");
    if (!def || def->montages.size() != 4) return;
    std::set<uint64_t> expected;
    for (const content::MontageRow& m : def->montages) expected.insert(expected_montage_id(m.soft_path));
    CHECK(expected.size() == 4 && expected.count(80184083730092395ull) == 1, "(the four expected ids: the recorded Dance04 and the three computed)");
    std::set<uint64_t> seen;
    bool dance04_item = false, dance04_export = false, other_computed = true;
    for (int i = 0; i < 24; ++i) {
        const size_t before = r.h.got.size();
        say_emote(r, "dance");
        r.play(0.6, false);
        uint64_t montage = 0;
        for (const auto& [eid, a] : r.m.abilities) {
            if (a.ability == kEmoteDance) montage = a.montage;
        }
        if (expected.count(montage)) seen.insert(montage);
        if (montage == 80184083730092395ull) {
            dance04_item = true;
            for (size_t c = before; c < r.h.got.size(); ++c) {
                bool carries = false;
                for (const auto& [ability, m] : chain_ability_items(r, r.h.got[c])) carries = carries || (ability == kEmoteDance && m == montage);
                if (!carries) continue;
                const std::vector<ExportRecord> records = export_records(r.h.got[c]);
                bool object = false, package = false;
                uint64_t package_id = 0;
                for (const ExportRecord& rec : records) {
                    if (rec.guid.ObjectId == 80184083730092395ull) {
                        object = rec.path == "All_EMO_M_Dance04_Montage";
                        package_id = rec.outer.ObjectId;
                    }
                }
                for (const ExportRecord& rec : records) {
                    if (package_id != 0 && rec.guid.ObjectId == package_id) package = rec.path == "/Game/Characters/Animations/00_EMO/All_EMO_M_Dance04_Montage" && package_id == 6184692607877003321ull;
                }
                dance04_export = dance04_export || (object && package);
            }
        } else if (montage != 0) {
            // the other three keep the computed recipe (the recorded id is the Dance04's alone)
            other_computed = other_computed && expected.count(montage) != 0 && montage != 80184083730092395ull;
        }
        r.play(1.0, false);   // the cooldown and the budget
    }
    CHECKF(seen.size() == 4, "all four dances came up (%zu of 4)", seen.size());
    CHECK(dance04_item, "the Dance04 item names the recorded object id 80184083730092395");
    CHECK(dance04_export, "the Dance04 export names its path with the recorded object and package ids (80184083730092395, 6184692607877003321)");
    CHECK(other_computed, "the other three dances keep their computed ids");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine)");
}

AOC_REGISTER_TEST(test_dance_uses_the_recorded_dance04_chain);

static void test_every_emote_word_plays_or_is_logged() {
    std::printf("every word of the emote list: the ones whose ability the tables hold play it, the others are logged and send nothing\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    Run r(0, true);
    r.play(0.5, false);
    const AbilityPipeline* pipe = r.h.s->pipeline();
    const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
    CHECK(pipe && cat, "(the session runs the generic ability runtime)");
    if (!pipe || !cat) return;
    size_t played = 0, unplayed = 0, wrong = 0, refused_by_runtime = 0;
    std::string wrong_words;
    std::set<uint64_t> item_montages;   // the montage ids the played items named
    const size_t chains_before = r.h.got.size();
    for (const auto& e : content::kEmoteCommands) {
        if (e.ability == content::kGuidRest) continue;   // /sit is the Rest: a test of its own
        const bool in_tables = cat->ability(e.ability) != nullptr;
        const size_t started = pipe->casts_started(), refused = pipe->refused(), items = r.m.abilities.size();
        g_log.str("");
        say_emote(r, std::string(e.command));
        r.play(0.6, false);
        refused_by_runtime += pipe->refused() - refused;
        bool good;
        if (in_tables) {
            const abilities::AbilityDef* d = cat->ability(e.ability);
            std::set<uint64_t> ids;
            if (d) {
                for (const content::MontageRow& m : d->montages) {
                    if (!m.soft_path.empty()) ids.insert(expected_montage_id(m.soft_path));
                }
            }
            bool item_ok = false;
            for (const auto& [eid, a] : r.m.abilities) {
                if (a.ability != e.ability) continue;
                item_ok = a.has_montage && ids.count(a.montage) != 0;
                if (item_ok) item_montages.insert(a.montage);
            }
            good = pipe->casts_started() == started + 1 && r.m.has_ability(e.ability) && cooldown_of(r, e.ability) && item_ok;
            if (good) ++played;
        } else {
            good = pipe->casts_started() == started && r.m.abilities.size() == items && !r.m.has_ability(e.ability) && text_has(emote_log(), "hold no ability for it");
            if (good) ++unplayed;
        }
        if (!good) {
            ++wrong;
            wrong_words += (wrong_words.empty() ? "" : ", ") + std::string(e.command);
        }
        r.play(1.0, false);   // the ability's cooldown is over before the next word, which can be the same ability
    }
    std::printf("  (%zu words played their ability, %zu have no ability in the tables%s%s)\n", played, unplayed, wrong_words.empty() ? "" : "; wrong: ", wrong_words.c_str());
    CHECK(wrong == 0, "each of the 53 words other than sit did what its ability's row in the tables says: it played with its montage, or was logged with nothing sent");
    CHECKF(played + unplayed == 53, "(53 words were tried: %zu played, %zu logged)", played, unplayed);
    std::set<uint64_t> exported;
    for (size_t i = chains_before; i < r.h.got.size(); ++i) {
        for (const ExportRecord& rec : export_records(r.h.got[i])) exported.insert(rec.guid.ObjectId);
    }
    bool all_exported = !item_montages.empty();
    for (uint64_t id : item_montages) all_exported = all_exported && exported.count(id) != 0;
    CHECKF(all_exported, "each of the %zu montages the played items named was exported to the client", item_montages.size());
    CHECK(refused_by_runtime == 0 && r.m.ends.empty(), "the runtime refused none of them and no ClientEndAbility was sent");
    CHECK(r.h.s->emotes_played() == played && r.h.s->emotes_refused() == unplayed && !r.h.s->abilities_failed() && r.m.unreadable == 0,
          "(the session's counts agree: played and refused, abilities on, every chain reads)");
}

AOC_REGISTER_TEST(test_every_emote_word_plays_or_is_logged);

static void test_a_sit_in_combat_is_refused_and_logged() {
    std::printf("/sit in combat: refused and logged, nothing is sent\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    WolfWorld ww;
    ww.level = 8;
    const svc::Character rec = bard25();
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground and a Wolf next to the player");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return;
    // the Wolf notices the player and bites; the player's in-combat flag follows
    bool in_combat = false;
    for (int i = 0; i < 150 && !in_combat; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
        in_combat = r.h.s->character_stats() && r.h.s->character_stats()->in_combat();
    }
    CHECK(in_combat, "(the Wolf fights the player: the player is in combat)");
    if (!in_combat) return;
    const size_t items = r.m.abilities.size(), ends = r.m.ends.size();
    g_log.str("");
    say_emote(r, "sit");
    for (int i = 0; i < 3; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    const std::string text = emote_log();
    CHECK(!r.m.has_ability(kRest) && r.m.abilities.size() == items && r.h.s->abilities() && !r.h.s->abilities()->resting(), "no Rest item arrives and the runtime does not rest");
    CHECK(r.m.ends.size() == ends, "the client is sent no ClientEndAbility");
    CHECK(text_has(text, "\"sit\" is refused: in combat"), "the refusal is logged with its reason");
    CHECK(r.h.s->emotes_played() == 0 && r.h.s->emotes_refused() == 1 && !r.h.s->abilities_failed(), "(the session counts it as refused)");
    // the other emotes have no such rule
    say_emote(r, "wave");
    for (int i = 0; i < 6; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    CHECK(r.m.has_ability(kEmoteWave), "a /wave in combat plays: Emote_Wave's record has no combat requirement");
}

AOC_REGISTER_TEST(test_a_sit_in_combat_is_refused_and_logged);

static void test_a_dead_character_plays_no_emote() {
    std::printf("a dead character plays no emote and sits no more: the refusal is logged, nothing is sent\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    Run r(0, true);
    r.play(0.5, false);
    r.h.s->take_damage(r.h.conn, 1.0e9, r.h.t);
    r.play(0.3, false);
    CHECK(r.h.s->dead(), "(the character is dead)");
    const size_t items = r.m.abilities.size();
    g_log.str("");
    say_emote(r, "wave");
    say_emote(r, "sit");
    r.play(0.8, false);
    const std::string text = emote_log();
    CHECK(!r.m.has_ability(kEmoteWave) && !r.m.has_ability(kRest) && r.m.abilities.size() == items, "no ability item arrives");
    CHECK(text_has(text, "\"wave\" is refused: the character is dead") && text_has(text, "\"sit\" is refused: the character is dead"), "both refusals are logged with their reason");
    CHECK(r.h.s->emotes_played() == 0 && r.h.s->emotes_refused() == 2 && !r.h.s->abilities_failed(), "(none played, two refused)");
}

AOC_REGISTER_TEST(test_a_dead_character_plays_no_emote);

static void test_a_damaged_emote_call_never_breaks_the_session() {
    std::printf("a ServerPlayEmote with any one bit flipped, and one cut short, never breaks the session\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    Run r(0, true);
    r.play(0.5, false);
    const BunchWriter good = emote_bunch("wave");
    CHECK(good.bit_pos() > 32, "(the call is written)");
    const size_t bits = good.bit_pos();
    size_t sent = 0;
    for (size_t bit = 0; bit < bits; ++bit) {
        r.h.client_sends({r.h.client.actor(r.h.pc_ch, flipped(good, bit), /*reliable=*/true)});
        r.h.flush();
        r.absorb();
        ++sent;
        r.play(0.3, false);   // the budget earns its tokens back, a cooldown ends
        if (r.h.s->abilities_failed() || r.h.s->failed()) break;
    }
    CHECKF(sent == bits, "each of the call's %zu bits was flipped once and sent", bits);
    CHECK(!r.h.s->abilities_failed() && !r.h.s->failed() && r.m.unreadable == 0, "the session is fine after them all: abilities on, not failed, every chain reads");
    CHECK(r.m.ends.empty(), "...and the client was sent no ClientEndAbility for any of them");
    CHECKF(r.h.s->emotes_played() + r.h.s->emotes_refused() + r.h.s->emotes_dropped() <= sent, "(the session looked at no more calls than were sent: %zu played, %zu refused, %zu dropped)",
           r.h.s->emotes_played(), r.h.s->emotes_refused(), r.h.s->emotes_dropped());
    // a call cut short: the first half of the bunch only
    BunchWriter cut;
    {
        std::vector<uint8_t> bytes(good.data(), good.data() + good.byte_size());
        cut.write_bit_range(bytes.data(), 0, bits / 2);
    }
    r.h.client_sends({r.h.client.actor(r.h.pc_ch, cut, /*reliable=*/true)});
    r.h.flush();
    r.absorb();
    r.play(0.3, false);
    CHECK(!r.h.s->abilities_failed() && !r.h.s->failed() && r.m.unreadable == 0, "a call cut in half changes nothing either");
}

AOC_REGISTER_TEST(test_a_damaged_emote_call_never_breaks_the_session);

static void test_the_rest_runtime_when_the_server_asks() {
    std::printf("the Rest when the server starts and ends it: no ClientEndAbility for a refusal, and a way to stand up in combat\n");
    Fixture fx;
    std::string error;
    const auto rules = fx.content->ability_rules(error);
    CHECK(rules.has_value(), "(the fixture's ability rules read)");
    if (!rules) return;
    AbilityRuntime rt(*rules, {{content::kGuidStamina, 100}, {content::kGuidMaxStamina, 100}, {kHealthRegen, 0.6}, {kManaRegen, 0.6}, {content::kGuidMaxHealth, 90}, {content::kGuidMaxMana, 90}, {kMoveSpeedMult, 1.0}}, 1000);
    TriggerInfo ask;
    ask.by_server = true;
    const auto ends_of = [&](const std::vector<AbilityEvent>& events) {
        std::vector<int32_t> out;
        for (const AbilityEvent& e : events)
            if (e.end_ability) out.push_back(*e.end_ability);
        return out;
    };
    // in combat the Rest is refused; asked by the server it ends nothing on the client, asked by the client it does
    rt.set_in_combat(true, 0.0);
    CHECK(rt.trigger(kRest, -1, 0.0, ask) == Refusal::InCombat, "in combat a Rest the server starts is refused");
    std::vector<AbilityEvent> events = rt.take_events();
    CHECK(events.size() == 1 && events[0].refusal && !events[0].end_ability, "...with a refusal event that tells the client nothing");
    CHECK(rt.trigger(kRest, 7, 0.1) == Refusal::InCombat && ends_of(rt.take_events()) == std::vector<int32_t>{7}, "a Rest the client asked for is refused with ClientEndAbility, as before");
    // mounted: the same
    rt.set_in_combat(false, 0.2);
    rt.set_mounted(true, 0.2);
    rt.take_events();
    CHECK(rt.trigger(kRest, -2, 0.3, ask) == Refusal::Mounted && ends_of(rt.take_events()).empty(), "mounted a Rest the server starts is refused and tells the client nothing");
    rt.set_mounted(false, 0.4);
    rt.take_events();
    // out of combat it starts
    CHECK(!rt.end_rest(0.45), "(nothing rests yet: there is nothing to end)");
    CHECK(rt.trigger(kRest, -3, 0.5, ask) == Refusal::None && rt.resting(), "out of combat it starts and the runtime rests");
    events = rt.take_events();
    bool not_predicted = false;
    for (const AbilityEvent& e : events)
        for (const bs::ActiveAbilityItem& a : e.change.abilities_added) not_predicted = not_predicted || (a.ability_guid == kRest && !a.branch_predicted);
    CHECK(not_predicted, "the item of a Rest the server starts says the client did not predict it (bBranchPredictedOnAutonomous 0), as the game's item of an emote does");
    // standing up works in combat
    rt.set_in_combat(true, 1.0);
    CHECK(rt.end_rest(1.0) && !rt.resting(), "ending the Rest works in combat: the character can always stand up");
    events = rt.take_events();
    bool jumped = false;
    for (const AbilityEvent& e : events)
        for (const bs::ActiveAbilityItem& a : e.change.abilities_changed) jumped = jumped || (a.ability_guid == kRest && a.may_jump_to_position && a.montage_position == AbilityRuntime::kRestSitEndPosition && a.animation_counter == 1 && !a.branch_predicted);
    CHECK(jumped, "...and the Rest's montage jumps to its end section (AnimationCounter 1), still an item the client did not predict");
    // a Rest the client asked for is predicted by it, as ever
    AbilityRuntime own(*rules, {{content::kGuidStamina, 100}, {content::kGuidMaxStamina, 100}, {kHealthRegen, 0.6}, {kManaRegen, 0.6}, {content::kGuidMaxHealth, 90}, {content::kGuidMaxMana, 90}, {kMoveSpeedMult, 1.0}}, 1000);
    CHECK(own.trigger(kRest, 4, 0.0) == Refusal::None, "(the client asks for a Rest of its own)");
    bool predicted = false;
    for (const AbilityEvent& e : own.take_events())
        for (const bs::ActiveAbilityItem& a : e.change.abilities_added) predicted = predicted || (a.ability_guid == kRest && a.branch_predicted);
    CHECK(predicted, "its item says it did predict it, as before");
    CHECK(!rt.end_rest(1.1), "a second end finds nothing to end");
    rt.tick(1.2);
    events = rt.take_events();
    CHECK(rt.refused() == 3 && !rt.resting(), "(three Rests were refused, none rests)");
}

AOC_REGISTER_TEST(test_the_rest_runtime_when_the_server_asks);

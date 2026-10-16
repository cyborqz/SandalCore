// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/test_worldd_character_setup.cpp
//
//  Plan E1: the character record -> PlayerSetup mapping (server/worldd/bootstrap/character_setup.h).
//
//  Proofs:
//   - the record's 32 hex digits are the capture's four Guid words (D8B2FE19 95FB4DF9 9B1E3958 6C2B2BD4);
//   - race, gender and class are the client's enums (plan B1): race 0-7 and gender 0-1 are taken as they are,
//     Tulnar and an id past the enum are refused as an unsupported race, and the class is the record's class_id
//     (ECharacterClass) looked up by its name; every race joins;
//   - customization (a): the capture's own CharacterCustomization (pkt#78, 26,861 bits) -> the client's
//     JSON form -> back equals the decoded capture value, member for member and bit for bit (floats
//     as float32 bits, 83 morphs and 30 sections in order, both decals and blend groups);
//   - customization (b): cyb's stored JSON (cyb_appearance.h: 41 keys) maps without error, shares what
//     it should with the capture (randomSeed, eyeColor, the 83 morph FNames: the first-letter and
//     "_0" -> number 1 rules), and the value encodes and decodes through the replicator's codec to the
//     last bit, and every pawn open of the setups below builds from it;
//   - the capture's record shape (RandomChar: its Guid, name, race 0, gender 0, class 2, archetype 17747, level 1,
//     the JSON of its customization, its spawn) wearing the Bard's 13 starter items gives a setup equal field by field to
//     captured_character_setup(), 442 stats and held items included (plan B3: the stat program on the real stat blocks), but
//     for the toast flag the record does not carry;
//   - plan B3: the stats of the setup are the engine's, laid out as the captured arrays (stats_setup_for); each class's starting kit at
//     level 1 gives the corpus's own digest (the Go engine's) and the mods and tags Go made from the same items;
//   - cyb-like records of every race 0-7 and both genders build a setup whose PlayerBootstrap is ok;
//   - the error cases.
//
//  The content.db of the unit tests is a small SQLite file (content_db_fixture.h); with AOC_CONTENT_DB set
//  to the real one (cd services && go run ./cmd/contentc) the capture's record and cyb are built against
//  it as well ("[skip]" otherwise).
// ============================================================================
#include "bootstrap/bootstrap.h"
#include "bootstrap/captured_setup.h"
#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "content_db_fixture.h"
#include "world_stats.h"
#include "cyb_appearance.h"
#include "gear_fixture.h"
#include "stats/gear.h"
#include "stats/stat_engine.h"
#include "stats_fixture.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/wire/packet_reader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace bs = aoc::worldd::bootstrap;
namespace content = aoc::worldd::content;
namespace stats = aoc::worldd::stats;
namespace svc = aoc::worldd::svc;
namespace repl = aoc::protocol::repl;
namespace emit = aoc::protocol::emit;
namespace wire = aoc::protocol::wire;
namespace fs = std::filesystem;
// == C O A | C-Y-B ==

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---- comparing two setups, field by field -----------------------------------------------------------------------------

static void diff_stat_array(std::vector<std::string>& d, const char* name, const bs::StatArray& a, const bs::StatArray& b) {
    if (a.array_key != b.array_key) d.push_back(std::string(name) + ".array_key");
    if (a.items.size() != b.items.size()) {
        d.push_back(std::string(name) + ".size");
        return;
    }
    for (size_t i = 0; i < a.items.size(); ++i) {
        const bs::StatItem &x = a.items[i], &y = b.items[i];
        if (x.id != y.id || x.record != y.record || x.extended != y.extended || x.base != y.base || x.equipment != y.equipment) {
            d.push_back(std::string(name) + "[" + std::to_string(i) + "].id/record/extended/base/equipment");
        }
        if (x.value != y.value) d.push_back(std::string(name) + "[record " + std::to_string(x.record) + "].value");
    }
}

/// The names of the PlayerSetup fields in which `a` and `b` differ (an empty list: equal in every field).
static std::vector<std::string> diff_setups(const bs::PlayerSetup& a, const bs::PlayerSetup& b) {
    std::vector<std::string> d;
    const auto note = [&](bool same, const char* name) {
        if (!same) d.push_back(name);
    };
    note(a.character_guid == b.character_guid, "character_guid");
    note(a.name == b.name, "name");
    note(a.race == b.race, "race");
    note(a.gender == b.gender, "gender");
    note(a.level == b.level, "level");
    note(a.class_record_guid == b.class_record_guid, "class_record_guid");
    note(a.customization == b.customization, "customization");
    note(a.shared_appearance_info_id == b.shared_appearance_info_id, "shared_appearance_info_id");
    note(std::bit_cast<uint64_t>(a.x) == std::bit_cast<uint64_t>(b.x), "x");
    note(std::bit_cast<uint64_t>(a.y) == std::bit_cast<uint64_t>(b.y), "y");
    note(std::bit_cast<uint64_t>(a.z) == std::bit_cast<uint64_t>(b.z), "z");
    note(std::bit_cast<uint64_t>(a.yaw_degrees) == std::bit_cast<uint64_t>(b.yaw_degrees), "yaw_degrees");
    diff_stat_array(d, "stats.owner", a.stats.owner, b.stats.owner);
    diff_stat_array(d, "stats.owner_proxy", a.stats.owner_proxy, b.stats.owner_proxy);
    diff_stat_array(d, "stats.everyone_proxy", a.stats.everyone_proxy, b.stats.everyone_proxy);
    note(a.stats.float_array_key == b.stats.float_array_key, "stats.float_array_key");
    bool floats = a.stats.float_everyone_proxy.size() == b.stats.float_everyone_proxy.size();
    for (size_t i = 0; floats && i < a.stats.float_everyone_proxy.size(); ++i) {
        floats = a.stats.float_everyone_proxy[i].id == b.stats.float_everyone_proxy[i].id &&
                 a.stats.float_everyone_proxy[i].record == b.stats.float_everyone_proxy[i].record;
    }
    note(floats, "stats.float_everyone_proxy");
    note(a.known_abilities == b.known_abilities, "known_abilities");
    bool records = a.records_to_load.size() == b.records_to_load.size();
    for (size_t i = 0; records && i < a.records_to_load.size(); ++i) {
        records = a.records_to_load[i].guid == b.records_to_load[i].guid && a.records_to_load[i].type_id == b.records_to_load[i].type_id;
    }
    note(records, "records_to_load");
    note(a.weapon_skill_line.guid == b.weapon_skill_line.guid && a.weapon_skill_line.name == b.weapon_skill_line.name, "weapon_skill_line");
    note(a.attuned_blocks == b.attuned_blocks, "attuned_blocks");
    bool held = a.held_items.array_key == b.held_items.array_key && a.held_items.items.size() == b.held_items.items.size();
    for (size_t i = 0; held && i < a.held_items.items.size(); ++i) {
        const bs::HeldItem &x = a.held_items.items[i], &y = b.held_items.items[i];
        held = x.id == y.id && x.slot == y.slot && x.item_guid == y.item_guid && x.enchantment == y.enchantment;
    }
    note(held, "held_items");
    note(a.appearance_ids == b.appearance_ids, "appearance_ids");
    note(a.orgs_at_war_toast == b.orgs_at_war_toast, "orgs_at_war_toast");
    note(a.login.name_option == b.login.name_option && a.login.unique_id == b.login.unique_id, "login");
    note(a.server_id == b.server_id, "server_id");
    note(std::bit_cast<uint64_t>(a.world_time) == std::bit_cast<uint64_t>(b.world_time), "world_time");
    note(a.player_id == b.player_id, "player_id");
    const emit::FIntrepidNetworkGUID* ga[] = {&a.guids.player_state, &a.guids.player_controller, &a.guids.global_gm_commands, &a.guids.game_state,
                                              &a.guids.pawn, &a.guids.base_character_info, &a.guids.combat_info, &a.guids.owner_info, &a.guids.backpack,
                                              &a.guids.equipment, &a.guids.quest_storage, &a.guids.reward_storage, &a.guids.appearance,
                                              &a.guids.resource_storage[0], &a.guids.resource_storage[1], &a.guids.resource_storage[2], &a.guids.collision_cylinder, &a.guids.alignment, &a.guids.ability, &a.guids.stats};
    const emit::FIntrepidNetworkGUID* gb[] = {&b.guids.player_state, &b.guids.player_controller, &b.guids.global_gm_commands, &b.guids.game_state,
                                              &b.guids.pawn, &b.guids.base_character_info, &b.guids.combat_info, &b.guids.owner_info, &b.guids.backpack,
                                              &b.guids.equipment, &b.guids.quest_storage, &b.guids.reward_storage, &b.guids.appearance,
                                              &b.guids.resource_storage[0], &b.guids.resource_storage[1], &b.guids.resource_storage[2], &b.guids.collision_cylinder, &b.guids.alignment, &b.guids.ability, &b.guids.stats};
    bool guids = true;
    for (size_t i = 0; i < std::size(ga); ++i) guids = guids && *ga[i] == *gb[i];
    note(guids, "guids");
    return d;
}

// ---- the capture's character as a record ------------------------------------------------------------------------------
// Cyb : C.O.A

/// The characters table's row of the capture's RandomChar: its id, name, race 0 Kaelar, gender 0 Male, class 2 Bard,
/// archetype 17747, level 1, the JSON form of its customization and its spawn.
static svc::Character captured_record(const bs::PlayerSetup& cap) {
    svc::Character c;
    c.id = "d8b2fe1995fb4df99b1e39586c2b2bd4";
    c.account_id = 1;
    c.name = "RandomChar";
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    const auto js = bs::customization_to_json(cap.customization);
    c.appearance_json = js ? *js : std::string();
    c.level = 1;
    c.has_position = true;
    c.position = {cap.x, cap.y, cap.z, static_cast<float>(cap.yaw_degrees)};
    return c;
}

static bs::SessionValues session_of(const bs::PlayerSetup& cap) {
    bs::SessionValues s;
    s.server_id = cap.server_id;
    s.world_time = cap.world_time;
    s.player_id = cap.player_id;
    s.guids = cap.guids;
    return s;
}

/// cyb: a level-1 Bard with the JSON as the character store returns it (jsonb text); race and gender are the record's
/// ids (ECharacterRace, ECharacterGender): cyb himself is 0 and 0, a Kaelar male.
static svc::Character cyb_record(int32_t race_id, int32_t gender_id, const std::string& json = cyb::kAppearanceJsonb) {
    svc::Character c;
    c.id = "bbec5c86eb101fe5f38c87f0fa22e7d0";
    c.account_id = 1;
    c.name = "cyb";
    c.race_id = race_id;
    c.gender_id = gender_id;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = json;
    c.level = 1;
    return c;
}

static std::string replaced(std::string s, const std::string& from, const std::string& to) {
    const size_t at = s.find(from);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

/// cyb's stored JSON (jsonb text) with its race and gender names changed (the JSON names them as well as the record).
static std::string cyb_json_of(const char* race, const char* gender) {
    std::string j = replaced(cyb::kAppearanceJsonb, "\"gender\": \"Male\"", std::string("\"gender\": \"") + gender + "\"");
    return replaced(j, "\"race\": \"Kaelar\"", std::string("\"race\": \"") + race + "\"");
}

/// Two customizations equal but for the order of the morph and section lists (a map's order is nothing the client sees;
/// PostgreSQL's jsonb reorders an object's keys).
static bool same_but_order(repl::CharacterCustomization a, repl::CharacterCustomization b) {
    for (repl::CharacterCustomization* c : {&a, &b}) {
        if (c->face_morph_weights) {
            std::sort(c->face_morph_weights->begin(), c->face_morph_weights->end(), [](const auto& x, const auto& y) {
                return std::tie(x.name.hardcoded, x.name.index, x.name.text, x.name.number) < std::tie(y.name.hardcoded, y.name.index, y.name.text, y.name.number);
            });
        }
        if (c->sections_values) {
            std::sort(c->sections_values->begin(), c->sections_values->end(), [](const auto& x, const auto& y) { return x.key < y.key; });
        }
    }
    return a == b;
}
// [CoA + C_Y_B]

/// The value through the replicator's codec: written, read back, written again.
static bool codec_round_trip(const repl::CharacterCustomization& c, size_t* bits_out = nullptr) {
    using repl::Value;
    emit::BunchWriter w;
    if (!repl::write_struct("FCharacterCustomizationSaveData", Value(c), w)) return false;
    wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    const auto back = repl::read_struct("FCharacterCustomizationSaveData", r);
    if (!back || r.pos() != w.bit_pos()) return false;
    const auto* v = back->get_if<repl::CharacterCustomization>();
    if (!v || !(*v == c)) return false;
    emit::BunchWriter w2;
    if (!repl::write_struct("FCharacterCustomizationSaveData", *back, w2) || w2.bit_pos() != w.bit_pos()) return false;
    for (size_t i = 0; i < w.byte_size(); ++i) {
        if (w.data()[i] != w2.data()[i]) return false;
    }
    if (bits_out) *bits_out = w.bit_pos();
    return true;
}

// ---- tests ------------------------------------------------------------------------------------------------------------------

static void test_tables() {
    std::printf("race, gender and class: the client's enums, taken as they are\n");
    bool races = true;
    for (int32_t id = 0; id <= 7; ++id) {
        const auto r = bs::client_race_from_id(id);
        races = races && r && static_cast<int>(*r) == id;
    }
    CHECK(races, "races 0 Kaelar to 7 Nikua are the wire's races, unchanged");
    CHECK(bs::client_race_from_id(0) == bs::ClientRace::Kaelar && bs::client_race_from_id(6) == bs::ClientRace::Dunir &&
              bs::client_race_from_id(4) == bs::ClientRace::Empyrean && bs::client_race_from_id(2) == bs::ClientRace::Vek,
          "race 0 is Kaelar, 2 Vek (not Kaelar: the old 2), 4 Empyrean, 6 Dunir (not 7)");
    bool others = true;
    for (int32_t id : {-1, 8, 9, 10, 11, 100}) others = others && !bs::client_race_from_id(id);
    CHECK(others, "Tulnar (8) and every id past the enum (-1, 9-11, 100) are nullopt");
    CHECK(bs::client_gender_from_id(0) == bs::ClientGender::Male && static_cast<int>(*bs::client_gender_from_id(0)) == 0, "gender 0 Male");
    CHECK(bs::client_gender_from_id(1) == bs::ClientGender::Female && static_cast<int>(*bs::client_gender_from_id(1)) == 1, "gender 1 Female");
    CHECK(!bs::client_gender_from_id(2) && !bs::client_gender_from_id(3) && !bs::client_gender_from_id(-1), "gender 2, 3 and -1 are unknown");
    const char* classes[] = {nullptr, "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"};
    bool class_names = true;
    for (int32_t id = 1; id <= 8; ++id) class_names = class_names && bs::client_class_name(id) && std::string(bs::client_class_name(id)) == classes[id];
    CHECK(class_names && !bs::client_class_name(0) && !bs::client_class_name(9) && !bs::client_class_name(-1),
          "ECharacterClass: 1 Ranger, 2 Bard, 3 Cleric, 4 Fighter, 5 Rogue, 6 Summoner, 7 Tank, 8 Mage; 0 and past 8 have no name");

    const char* names[] = {"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua", "Tulnar"};
    bool names_ok = true;
    for (int i = 0; i < 9; ++i) {
        const auto race = static_cast<bs::ClientRace>(i);
        names_ok = names_ok && std::string(bs::client_race_name(race)) == names[i] && bs::client_race_from_name(names[i]) == race;
    }
    CHECK(names_ok && !bs::client_race_name(static_cast<bs::ClientRace>(9)) && !bs::client_race_from_name("Aela") && !bs::client_race_from_name("kaelar"),
          "the enum's nine names (ECharacterRace 0-8, enum_values.txt) both ways; a tenth value and a stranger have none");
    CHECK(std::string(bs::client_gender_name(bs::ClientGender::Female)) == "Female" && bs::client_gender_from_name("Male") == bs::ClientGender::Male &&
              !bs::client_gender_from_name("male") && !bs::client_gender_name(static_cast<bs::ClientGender>(2)),
          "gender names Male and Female both ways");

    // the capture's Guid
    const auto w = bs::character_guid_words("d8b2fe1995fb4df99b1e39586c2b2bd4");
    CHECK(w && (*w)[0] == 3635609113u && (*w)[1] == 2516274681u && (*w)[2] == 2602449240u && (*w)[3] == 1814768596u,
          "d8b2fe1995fb4df99b1e39586c2b2bd4 -> 3635609113, 2516274681, 2602449240, 1814768596 (pkt#78 h3-h6)");
    CHECK(bs::character_guid_words("D8B2FE1995FB4DF99B1E39586C2B2BD4") == w, "upper case reads the same");
    CHECK(!bs::character_guid_words("") && !bs::character_guid_words("d8b2fe1995fb4df99b1e39586c2b2bd") && !bs::character_guid_words("d8b2fe1995fb4df99b1e39586c2b2bd40") &&
              !bs::character_guid_words("g8b2fe1995fb4df99b1e39586c2b2bd4") && !bs::character_guid_words("d8b2fe19-95fb-4df9-9b1e-39586c2b2bd4"),
          "31 or 33 digits, a non-hex digit and the dashed form are refused");
    CHECK(bs::body_section_name(0) == std::string("None") && bs::body_section_name(1) == std::string("LowerArms") &&
              bs::body_section_name(28) == std::string("RacialGroup") && bs::body_section_name(29) == std::string("cCount") && !bs::body_section_name(30),
          "the body section enum: None 0, LowerArms 1 ... RacialGroup 28, cCount 29");
}

static void test_capture_roundtrip(const bs::PlayerSetup& cap) {
    std::printf("customization (a): the capture's own value -> the client's JSON -> back\n");
    const repl::CharacterCustomization& c = cap.customization;
    CHECK(c.deltas.size() == 20 && c.decals && c.decals->size() == 2 && c.blend_groups && c.blend_groups->size() == 2 && !(*c.blend_groups)[0] &&
              c.sections_values && c.sections_values->size() == 30 && c.face_morph_weights && c.face_morph_weights->size() == 83 && c.race_gender &&
              c.random_seed == 1454u,
          "the capture's value: 20 scalar deltas, 2 decals, 2 blend groups (the first not sent), 30 sections, 83 morphs, seed 1454");
    const auto js = bs::customization_to_json(c);
    CHECK(js.has_value(), "it has a JSON form");
    if (!js) return;
    std::printf("    the JSON form is %zu characters\n", js->size());
    const auto back = bs::customization_from_json(*js, bs::ClientRace::Kaelar, bs::ClientGender::Male);
    CHECK(back.value.has_value() && back.error.empty(), "the JSON form reads");
    if (!back.value) {
        std::printf("    %s\n", back.error.c_str());
        return;
    }
    CHECK(*back.value == c, "...to a value equal to the decoded capture's, member for member and bit for bit");
    CHECK(back.ignored_keys.empty(), "...with no member left unread");
    CHECK(back.value->deltas == c.deltas && back.value->face_morph_weights == c.face_morph_weights && back.value->sections_values == c.sections_values &&
              back.value->decals == c.decals && back.value->blend_groups == c.blend_groups && back.value->race_gender == c.race_gender,
          "(deltas, morphs in order, sections in order, decals, blend groups, Race and Gender each equal)");
    CHECK(codec_round_trip(*back.value), "...and it encodes and decodes through the replicator's codec to the last bit");
    // the JSON of the original is also JSON of the same value: re-serialising what we read gives the same text
    const auto again = bs::customization_to_json(*back.value);
    CHECK(again && *again == *js, "writing the value read back gives the same JSON text");
// CoA : C_Y_B

    // a value with every member kind present survives too
    repl::CharacterCustomization full = c;
    full.preset_guid.reset();
    full.helmet_visible = true;
    full.cape_visible = false;
    full.deltas[repl::CustomizationField::NailOpacity] = std::bit_cast<uint32_t>(0.25f);
    full.deltas[repl::CustomizationField::SkinSet] = static_cast<uint32_t>(-3);
    full.deltas[repl::CustomizationField::RacialHorns] = 0x8000000000000001ull;      // an id with the top bit set
    const auto fj = bs::customization_to_json(full);
    const auto fb = fj ? bs::customization_from_json(*fj, bs::ClientRace::Kaelar, bs::ClientGender::Male) : bs::CustomizationFromJson{};
    CHECK(fb.value && *fb.value == full, "booleans, a negative SkinSet and an id above 2^63 come back equal");
}

static void test_cyb_customization() {
    std::printf("customization (b): cyb's stored JSON\n");
    const std::string json = cyb::kAppearanceJson;
    const auto r = bs::customization_from_json(json, bs::ClientRace::Kaelar, bs::ClientGender::Male);
    CHECK(r.value.has_value() && r.error.empty(), "cyb's 41-key JSON maps without error");
    if (!r.value) {
        std::printf("    %s\n", r.error.c_str());
        return;
    }
    const repl::CharacterCustomization& c = *r.value;
    CHECK(r.ignored_keys.empty(), "every key is read: none ignored (class, gender and race are read as well)");
    CHECK(c.deltas.size() == 30 && !c.preset_guid && c.random_seed == 1454u && c.race_gender && c.race_gender->race == 0 && c.race_gender->gender == 0,
          "all 30 scalar members, no preset, seed 1454, Race 0 Gender 0 (Kaelar, Male: the capture's bytes)");
    CHECK(c.deltas.at(repl::CustomizationField::EyeColor) == 108238840485496ull && c.deltas.at(repl::CustomizationField::SkinSet) == 1u &&
              c.deltas.at(repl::CustomizationField::ScleraShape) == 6064630690389884928ull && c.deltas.at(repl::CustomizationField::RacialHorns) == 0 &&
              c.deltas.at(repl::CustomizationField::SkinColorHue) == std::bit_cast<uint32_t>(0.85246866941452026f) &&
              c.deltas.at(repl::CustomizationField::NormalDetailStrength01) == 0,
          "ids from decimal strings (eyeColor 108238840485496), floats as float32 bits, skinSet 1");
    CHECK(c.decals && c.decals->empty() && c.blend_groups && c.blend_groups->empty() && c.helmet_visible == true && c.cape_visible == true,
          "decalData and decalBlendGroups [] (sent as empty arrays), both visibility flags true");
    CHECK(c.sections_values && c.sections_values->size() == 30 && c.face_morph_weights && c.face_morph_weights->size() == 84, "30 body sections, 84 face morphs");
    bool section_keys = c.sections_values.has_value();
    for (size_t i = 0; section_keys && i < c.sections_values->size(); ++i) section_keys = (*c.sections_values)[i].key == i;
    CHECK(section_keys, "the section keys are 0..29 in order: the enum names in the JSON's order (none, lowerArms ... racialGroup, cCount)");

    // the capture's own value, for what cyb shares with it
    const auto cap = bs::captured_character_setup();
    if (cap) {
        const repl::CharacterCustomization& k = cap->customization;
        CHECK(k.random_seed == c.random_seed && k.deltas.at(repl::CustomizationField::EyeColor) == c.deltas.at(repl::CustomizationField::EyeColor),
              "randomSeed 1454 and eyeColor are the capture's (transcript section 0)");
        // the capture's 83 morph FNames are cyb's keys with the first letter capitalised and "_N" split off as number N+1
        size_t found = 0;
        for (const auto& m : *k.face_morph_weights) {
            for (const auto& n : *c.face_morph_weights) {
                if (n.name == m.name) {
                    ++found;
                    break;
                }
            }
        }
        CHECK(found == 83, "all 83 captured morph FNames (text and number) are among cyb's: the capitalisation and '_0' -> number 1 rules match the capture");
        CHECK(k.sections_values && k.sections_values->size() == c.sections_values->size() && (*k.sections_values)[5].key == (*c.sections_values)[5].key,
              "the captured sections are the same 30 keys");
    }

    size_t bits = 0;
    CHECK(codec_round_trip(c, &bits), "the value encodes and decodes through the replicator's codec to the last bit");
    std::printf("    cyb's CharacterCustomization is %zu bits on the wire\n", bits);
    CHECK(repl::struct_accepts("FCharacterCustomizationSaveData", repl::Value(c)), "struct_accepts takes it");

    // the same JSON as the character store returns it: PostgreSQL's jsonb reorders the keys (by length, then bytes) and
    // respaces the text; the value is the same but for the order of the two maps
    const std::string jsonb = cyb::kAppearanceJsonb;
    CHECK(jsonb != json && jsonb.find("\"race\": \"Kaelar\", \"class\"") == 1, "kAppearanceJsonb is a different text (its first keys are race, class, gender)");
    const auto j = bs::customization_from_json(jsonb, bs::ClientRace::Kaelar, bs::ClientGender::Male);
    CHECK(j.value.has_value() && j.error.empty() && j.ignored_keys.empty(), "the jsonb form maps without error and with no key ignored");
    if (j.value) {
        CHECK(!(*j.value == c), "...but its morphs and sections come in another order than the client's");
        CHECK(same_but_order(*j.value, c), "...and are the same value but for that order (30 scalar members, 84 morphs, 30 sections)");
        CHECK(codec_round_trip(*j.value), "...which also encodes and decodes to the last bit");
    }
}
// (C O A ~ Cyb)

static void test_customization_errors() {
    std::printf("customization: what is refused\n");
    const auto read = [](const std::string& json, bs::ClientRace race = bs::ClientRace::Kaelar, bs::ClientGender g = bs::ClientGender::Male) {
        return bs::customization_from_json(json, race, g);
    };
    const std::string cyb_json = cyb::kAppearanceJson;
    auto r = read("");
    CHECK(!r.value && contains(r.error, "not valid JSON"), "an empty string is not JSON");
    r = read("{\"presetGuid\":");
    CHECK(!r.value && contains(r.error, "not valid JSON"), "cut-off JSON is refused");
    r = read("[1,2]");
    CHECK(!r.value && contains(r.error, "not a JSON object"), "an array is not a customization");
    r = read("{}");
    CHECK(r.value && r.value->deltas.empty() && r.value->race_gender && !r.value->decals && !r.value->sections_values, "{} is a customization with no member: all defaults, with Race and Gender");
    r = read(std::string(70000, ' ') + "{}");
    CHECK(!r.value && contains(r.error, "too large"), "JSON over kMaxAppearanceBytes (64 KB) is refused before it is parsed, valid or not");
    r = read(std::string(bs::kMaxAppearanceBytes - 2, ' ') + "{}");
    CHECK(r.value.has_value(), "(exactly kMaxAppearanceBytes is still read)");

    r = read(replaced(cyb_json, "\"presetGuid\":\"0\"", "\"presetGuid\":\"108136976045690\""));
    CHECK(!r.value && contains(r.error, "presetGuid") && contains(r.error, "preset table"), "a non-zero presetGuid is refused: whether Race and Gender are on the wire is not known");
    r = read(replaced(cyb_json, "\"skinSet\":1", "\"skinSet\":\"one\""));
    CHECK(!r.value && contains(r.error, "skinSet"), "a member of the wrong type names the member");
    r = read(replaced(cyb_json, "\"skinSet\":1", "\"skinSet\":3000000000"));
    CHECK(!r.value && contains(r.error, "skinSet") && contains(r.error, "32 bits"), "skinSet beyond 32 bits is refused");
    r = read(replaced(cyb_json, "\"eyeColor\":\"108238840485496\"", "\"eyeColor\":\"12ab\""));
    CHECK(!r.value && contains(r.error, "eyeColor"), "an id that is not a decimal number is refused");
    r = read(replaced(cyb_json, "\"eyeColor\":\"108238840485496\"", "\"eyeColor\":\"99999999999999999999999\""));
    CHECK(!r.value && contains(r.error, "eyeColor"), "an id beyond 64 bits is refused");
    r = read(replaced(cyb_json, "\"headHairLength\":0.7976233959197998", "\"headHairLength\":1e300"));
    CHECK(!r.value && contains(r.error, "headHairLength") && contains(r.error, "finite"), "a float that overflows float32 is refused");
    r = read(replaced(cyb_json, "\"bIsHelmetVisible\":true", "\"bIsHelmetVisible\":1"));
    CHECK(!r.value && contains(r.error, "bIsHelmetVisible"), "a visibility flag that is not a boolean is refused");
    r = read(replaced(cyb_json, "\"lowerArms\":", "\"lowerLegs\":"));
    CHECK(!r.value && contains(r.error, "lowerLegs") && contains(r.error, "body section"), "an unknown body section is refused by name");
    r = read(replaced(cyb_json, "\"none\":{\"x\":", "\"none\":{\"q\":"));
    CHECK(!r.value && contains(r.error, "sectionsValues.none.x"), "a section without x is refused");
    r = read(replaced(cyb_json, "\"decalData\":[]", "\"decalData\":[{\"guid\":\"1\"}]"));
    CHECK(!r.value && contains(r.error, "decalData[0]"), "a decal missing members is refused");
    r = read(replaced(cyb_json, "\"decalBlendGroups\":[]", "\"decalBlendGroups\":{}"));
    CHECK(!r.value && contains(r.error, "decalBlendGroups"), "decalBlendGroups that is not an array is refused");
    r = read(replaced(cyb_json, "\"faceMorphWeightMaps\":{", "\"faceMorphWeightMaps\":{\"Nose_Bridge_Hooked_0\":0.5,"));
    CHECK(!r.value && contains(r.error, "cannot carry"), "a morph named twice (the JSON key and its capitalised twin) is refused before the wire");
    r = read(replaced(cyb_json, "\"nose_Bridge_Hooked_0\":0", "\"nose_Bridge_Hooked_0\":\"high\""));
    CHECK(!r.value && contains(r.error, "nose_Bridge_Hooked_0"), "a morph weight that is not a number names the morph");

    r = read(cyb_json, bs::ClientRace::Dunir, bs::ClientGender::Male);
    CHECK(!r.value && contains(r.error, "Kaelar") && contains(r.error, "Dunir"), "the JSON's race against the record's: a Kaelar appearance for a Dunir record is refused");
    r = read(cyb_json, bs::ClientRace::Kaelar, bs::ClientGender::Female);
    CHECK(!r.value && contains(r.error, "Male") && contains(r.error, "Female"), "the JSON's gender against the record's likewise");
    r = read(replaced(cyb_json, "\"race\":\"Kaelar\"", "\"race\":\"Elf\""));
    CHECK(!r.value && contains(r.error, "'race'"), "a race name that is none of the nine is refused");
    r = read(replaced(cyb_json, "\"class\":\"Bard\"", "\"class\":7"));
    CHECK(!r.value && contains(r.error, "'class'"), "a class that is not text is refused");

    r = read(replaced(cyb_json, "\"nailOpacity\":0", "\"nailOpacity\":0,\"hairBand\":3"));
    CHECK(r.value && r.ignored_keys.size() == 1 && r.ignored_keys[0] == "hairBand", "a member this server does not know is ignored and listed");
    // the JSON without race, gender and class (a customization from another source) still maps
    r = read(replaced(replaced(replaced(cyb_json, ",\"gender\":\"Male\"", ""), ",\"race\":\"Kaelar\"", ""), ",\"class\":\"Bard\"", ""));
    CHECK(r.value && r.ignored_keys.empty() && r.value->race_gender && r.value->race_gender->race == 0, "without race, gender and class keys the arguments decide");

    // toJSON refuses what JSON cannot hold
    repl::CharacterCustomization nan;
    nan.deltas[repl::CustomizationField::SkinColorHue] = std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN());
    CHECK(!bs::customization_to_json(nan).has_value(), "a NaN float has no JSON form");
    repl::CharacterCustomization hard;
    hard.face_morph_weights = std::vector<repl::CustomizationMorph>{{repl::NameValue::hardcoded_name(5), 0}};
    CHECK(!bs::customization_to_json(hard).has_value(), "a hardcoded morph name has no JSON form");
}

// ---- the corpus shared with the character service ---------------------------------------------------------------------------

/// The corpus file: next to the working directory's fixtures (scripts\run_tests.ps1 runs from the repo root), else by this
/// source file's place in the tree.
static std::optional<std::string> read_corpus() {
    const fs::path candidates[] = {
        fs::path("fixtures") / "customization_cases.json",
        fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "fixtures" / "customization_cases.json",
    };
    for (const fs::path& p : candidates) {
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }
    return std::nullopt;
}

/// fixtures/customization_cases.json (services/internal/customization: TestCorpusIsWhatTheServiceAccepts reads the same file).
/// Each case is a race, gender, class and customization JSON, with 'accepted' (what the character service's Check says) and
/// 'worldd' (what customization_from_json must make of the text that is stored). The rule: a character the service accepts
/// joins, so every accepted case is built; and every case is what 'worldd' says, so what the service refuses because worldd
/// would is refused here too.
static void test_customization_corpus() {
    std::printf("the customization corpus shared with the character service (fixtures/customization_cases.json)\n");
    const auto text = read_corpus();
    if (!text) {
        std::printf("  [FAIL] fixtures/customization_cases.json is not there (looked in the working directory and by the source's place)\n");
        g_fail++;
        return;
    }
    using ojson = nlohmann::ordered_json;
    const ojson file = ojson::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (file.is_discarded() || !file.contains("cases") || !file["cases"].is_array()) {
        std::printf("  [FAIL] the corpus is not JSON with a 'cases' array\n");
        g_fail++;
        return;
    }
    size_t cases = 0, accepted = 0, built = 0, refused = 0, wrong = 0;
    for (const ojson& c : file["cases"]) {
        ++cases;
        const std::string name = c.value("name", "?");
        std::string sent;
        if (c.contains("text")) {
            sent = c["text"].get<std::string>();
        } else if (c.contains("text_repeat")) {
            const ojson& r = c["text_repeat"];
            sent = r["prefix"].get<std::string>();
            const std::string unit = r["unit"].get<std::string>();
            for (int i = 0; i < r["count"].get<int>(); ++i) {
                std::string u = unit;
                for (size_t at; (at = u.find("{n}")) != std::string::npos;) u.replace(at, 3, std::to_string(i));
                sent += u;
            }
            sent += r["suffix"].get<std::string>();
        } else {
            sent = c["customization"].dump();
        }
        const std::string stored = c.contains("stored") ? c["stored"].dump() : sent;
        const auto race = bs::client_race_from_id(c["race"].get<int32_t>());
        const auto gender = bs::client_gender_from_id(c["gender"].get<int32_t>());
        if (!race || !gender) {
            std::printf("  [FAIL] %s: race %d gender %d is not a race and gender\n", name.c_str(), c["race"].get<int>(), c["gender"].get<int>());
            ++wrong;
            continue;
        }
        const bool want_built = c["worldd"].get<std::string>() == "builds";
        const bool is_accepted = c["accepted"].get<bool>();
        const auto r = bs::customization_from_json(stored, *race, *gender);
        if (r.value.has_value() != want_built) {
            std::printf("  [FAIL] %s: worldd %s it, the corpus says it %s (%s)\n", name.c_str(), r.value ? "builds" : "refuses",
                        want_built ? "builds" : "refuses", r.error.c_str());
            ++wrong;
            continue;
        }
        if (is_accepted && !r.value) {
            std::printf("  [FAIL] %s: the service accepts it and worldd refuses it\n", name.c_str());
            ++wrong;
            continue;
        }
        // a stored form that is not the sent one is a real preset made 0: worldd takes only the stored one
        if (c.contains("stored") && bs::customization_from_json(sent, *race, *gender).value.has_value()) {
            std::printf("  [FAIL] %s: worldd builds the sent form, which has a preset: Normalize would not be needed\n", name.c_str());
            ++wrong;
            continue;
        }
        accepted += is_accepted;
        built += r.value.has_value();
        refused += !r.value.has_value();
    }
    std::printf("  %zu cases: %zu accepted by the service, worldd builds %zu and refuses %zu\n", cases, accepted, built, refused);
    CHECK(cases > 300, "the corpus has the 125 retail customizations, cyb and the hostile cases");
    CHECK(wrong == 0, "worldd builds exactly the cases the corpus says, and every case the service accepts");
    CHECK(accepted > 130 && refused > 100, "the corpus has accepted cases (cyb, the retail ones) and cases worldd refuses");
}
// == C.O.A | Cyb ==

// ---- the stats (plan B3) ---------------------------------------------------------------------------------------------------------

/// A class's starting kit as the character's items: the equipped ones in their slots, quality 1 (the records' default).
static std::vector<svc::InventoryItem> kit_items(const gear_fixture::KitItem* kit, int count) {
    std::vector<svc::InventoryItem> out;
    for (int i = 0; i < count; ++i) {
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Equipment;
        it.slot = kit[i].slot;
        it.item_record_id = kit[i].record;
        char guid[33];
        std::snprintf(guid, sizeof guid, "%08x%08x%08x%08x", 0xB0000000u + static_cast<unsigned>(i), 0x1000u + static_cast<unsigned>(i), 0x2000u + static_cast<unsigned>(i),
                      static_cast<unsigned>(i) + 1);
        it.instance_guid = guid;
        it.quality = 1;
        it.durability = 11;
        it.created_at_ms = 1'771'299'474'000ll;
        out.push_back(it);
    }
    return out;
}

/// The capture's character wearing the Bard's kit.
static svc::Character captured_record_with_kit(const bs::PlayerSetup& cap) {
    svc::Character c = captured_record(cap);
    c.items = kit_items(gear_fixture::kBardKit, static_cast<int>(std::size(gear_fixture::kBardKit)));
    return c;
}

static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// The kit of a class as worn items.
static std::vector<stats::WornItem> kit_worn(const gear_fixture::ClassKit& k) {
    std::vector<stats::WornItem> w;
    for (int i = 0; i < k.count; ++i) w.push_back({k.items[i].record, 1, k.items[i].slot});
    return w;
}

static std::string mods_difference(const std::vector<stats::StatMod>& got, const stats_fixture::CorpusCase& want) {
    if (got.size() != want.mods.size()) return std::to_string(got.size()) + " mods, the corpus has " + std::to_string(want.mods.size());
    for (size_t i = 0; i < got.size(); ++i) {
        uint64_t b;
        std::memcpy(&b, &got[i].value, 8);
        if (got[i].stat != want.mods[i].first || b != want.mods[i].second) return "mod " + std::to_string(i) + " (stat " + std::to_string(got[i].stat) + ")";
    }
    return {};
}

static std::string tags_difference(std::vector<stats::EquippedTag> got, const stats_fixture::CorpusCase& want) {
    std::vector<std::pair<std::string, std::string>> a, b = want.tags;
    for (const auto& t : got) a.emplace_back(t.slot, t.tag);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) return std::to_string(a.size()) + " tags, the corpus has " + std::to_string(b.size());
    return {};
}
// CoA.C-Y-B

/// Runs the engine as the session does (no state) and checks its output against what Go wrote for the case: the digest of every stat.
static bool matches_corpus_digest(const bs::StatContext& ctx, const stats_fixture::CorpusCase& c, const stats::GearInputs& g, std::string& why) {
    stats::StatInputs in;
    in.class_enum = c.class_enum;
    in.level = c.level;
    in.mods = g.mods;
    in.equipped = g.equipped;
    stats::StatResult res;
    std::string err;
    if (!stats::run_stats(*ctx.bundle, in, stats::StatState{}, res, err)) {
        why = err;
        return false;
    }
    if (stats_fixture::digest(res) != c.digest || res.stats.size() != c.members || res.faults != c.faults || res.ignored_mods != c.ignored) {
        why = "digest " + std::to_string(stats_fixture::digest(res)) + " vs " + std::to_string(c.digest) + ", " + std::to_string(res.stats.size()) + " stats vs " +
              std::to_string(c.members) + ", ignored " + std::to_string(res.ignored_mods) + " vs " + std::to_string(c.ignored);
        return false;
    }
    return true;
}

static void test_kits_against_go(const content::ContentDb& db) {
    std::printf("each class's starting kit at level 1: the stats are the Go engine's, from the real stat blocks\n");
    const bs::StatContext& ctx = world_stats::of(db);
    const stats_fixture::Corpus& corpus = stats_fixture::corpus();
    std::string first_bad;
    int ok = 0;
    for (const gear_fixture::ClassKit& k : gear_fixture::kClassKits) {
        const std::string name = "kit_" + lower(k.name) + "_l1";
        const stats_fixture::CorpusCase* c = corpus.find(name);
        if (!c) {
            CHECK(false, (name + " is in the corpus").c_str());
            continue;
        }
        // the corpus's items are the kit
        bool same_items = c->items.size() == static_cast<size_t>(k.count);
        for (int i = 0; same_items && i < k.count; ++i) same_items = c->items[static_cast<size_t>(i)].record == k.items[i].record && c->items[static_cast<size_t>(i)].quality == 1;
        CHECK(same_items && !c->half_up, (std::string(k.name) + ": the corpus case wears this kit at quality 1, unrounded").c_str());
        const stats::GearInputs none = stats::gear_inputs(*ctx.gear, kit_worn(k), stats::Rounding::None);
        const stats::GearInputs half = stats::gear_inputs(*ctx.gear, kit_worn(k), stats::Rounding::HalfUp);
        std::string why = mods_difference(none.mods, *c);
        if (why.empty()) why = tags_difference(none.equipped, *c);
        if (why.empty() && !matches_corpus_digest(ctx, *c, none, why)) why = "digest: " + why;
        CHECK(why.empty(), (std::string(k.name) + ": the mods, the tags and the digest of every stat equal Go's").c_str());
        if (!why.empty()) std::printf("    %s\n", why.c_str());
        stats::GearInputs half_without_durability = half, none_without_durability = none;
        for (stats::GearInputs* g : {&half_without_durability, &none_without_durability}) {
            g->mods.erase(std::remove_if(g->mods.begin(), g->mods.end(), [](const stats::StatMod& m) { return m.stat == 6064629444242636800ll; }), g->mods.end());
        }
        bool whole = half_without_durability.mods.size() == none_without_durability.mods.size();
        for (size_t i = 0; whole && i < half_without_durability.mods.size(); ++i) {
            whole = half_without_durability.mods[i].stat == none_without_durability.mods[i].stat && half_without_durability.mods[i].value == none_without_durability.mods[i].value;
        }
        CHECK(whole && none.unknown_items == 0 && none.unknown_sets == 0,
              (std::string(k.name) + ": the kit's stat values are whole, so half-up rounding gives the same mods (but for MaxDurability's 37.5); every item is known").c_str());
        ok += why.empty();
    }
    CHECK(ok == 8, "all eight classes' kits equal Go's");
}

static void test_stats_setup_for() {
    std::printf("stats_setup_for: the arrays from the engine's stats\n");
    const auto value = [](int64_t guid, uint8_t rep, float v, uint8_t flags = 0) {
        stats::StatValue s;
        s.guid = guid;
        s.replication = rep;
        s.flags = flags;
        s.value = v;
        s.base = v - 1;
        s.equip = 1;
        return s;
    };
    // a layout like the captured one: an Owner array with ids 2, 4, 9 (key 12), an OwnerProxy with 3 (key 3), an EveryoneProxy with 5 (key 7), a float item
    bs::StatsSetup layout;
    layout.owner = {12, {{2, 100, 0, false, 0, 0}, {4, 101, 0, false, 0, 0}, {9, 102, 0, true, 0, 0}}};
    layout.owner_proxy = {3, {{3, 200, 0, false, 0, 0}}};
    layout.everyone_proxy = {7, {{5, 300, 0, false, 0, 0}}};
    layout.float_array_key = 4;
    layout.float_everyone_proxy = {{1, 900}};
    std::string error;

    // 101 is gone (another class), 103 is the new class's resource, 104 is ProxyOnly (not sent), 105 is not replicated, 900 is the float array's
    std::vector<stats::StatValue> v = {value(100, 1, 1.5f), value(102, 1, 2.5f, stats::kStatBuckets), value(103, 1, 3.5f), value(104, 6, 4.5f),
                                       value(105, 0, 5.5f), value(200, 5, 6.5f), value(300, 7, 7.5f), value(301, 7, 8.5f),
                                       value(900, 7, 0, stats::kStatFloatArray)};
    const auto out = bs::stats_setup_for(v, layout, error);
    CHECK(out.has_value(), "the arrays are made");
    if (!out) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    CHECK(out->owner.items.size() == 3 && out->owner.items[0].id == 2 && out->owner.items[0].record == 100 && out->owner.items[1].id == 9 &&
              out->owner.items[1].record == 102 && out->owner.items[2].id == 10 && out->owner.items[2].record == 103,
          "Owner: the layout's items still there keep their ids and order (2, 9), the class's own resource takes the next id (10), 101 is gone");
    CHECK(out->owner.array_key == 12, "...and the key stays the layout's 12");
    CHECK(out->owner.items[0].value == std::bit_cast<uint32_t>(1.5f) && !out->owner.items[0].extended, "a stat's value is its float bits");
    CHECK(out->owner.items[1].extended && out->owner.items[1].base == std::bit_cast<uint32_t>(1.5f) && out->owner.items[1].equipment == std::bit_cast<uint32_t>(1.0f),
          "a stat that replicates base and equipment carries both");
    CHECK(out->owner_proxy.items.size() == 1 && out->owner_proxy.items[0].id == 3 && out->owner_proxy.array_key == 3, "OwnerProxy: 200 keeps id 3 and the key");
    CHECK(out->everyone_proxy.items.size() == 2 && out->everyone_proxy.items[1].id == 6 && out->everyone_proxy.items[1].record == 301 && out->everyone_proxy.array_key == 7,
          "EveryoneProxy: 300 keeps 5, 301 takes 6, the key stays 7");
    CHECK(out->float_everyone_proxy.size() == 1 && out->float_everyone_proxy[0].id == 1 && out->float_everyone_proxy[0].record == 900 && out->float_array_key == 4,
          "the float array stat keeps its item");
    bool placed = true;
    for (const bs::StatArray* a : {&out->owner, &out->owner_proxy, &out->everyone_proxy}) {
        for (const bs::StatItem& it : a->items) placed = placed && it.record != 104 && it.record != 105 && it.record != 900;
    }
    CHECK(placed, "a stat of audience 6 (other players) or 0, and the float array's stat, are in none of the Int32 arrays");
// [C-O-A - Cyb]

    // a stat twice, a stat that is not finite
    std::vector<stats::StatValue> twice = {value(100, 1, 1), value(100, 1, 2)};
    CHECK(!bs::stats_setup_for(twice, layout, error) && contains(error, "twice"), "a stat twice is refused");
    std::vector<stats::StatValue> nan = {value(100, 1, std::numeric_limits<float>::quiet_NaN())};
    CHECK(!bs::stats_setup_for(nan, layout, error) && contains(error, "finite"), "a NaN is refused");

    // a new float array stat gets the next id
    std::vector<stats::StatValue> two_floats = {value(900, 7, 0, stats::kStatFloatArray), value(901, 7, 0, stats::kStatFloatArray)};
    const auto f = bs::stats_setup_for(two_floats, layout, error);
    CHECK(f && f->float_everyone_proxy.size() == 2 && f->float_everyone_proxy[1].id == 2 && f->float_everyone_proxy[1].record == 901, "a second float array stat takes the next id");
}

/// The mounts milestone, stage 3 fix 1: the abilities the carried items bring.  A mount item's Summon_Mount is in the join's ReplicatedRecordsToLoad (before the
/// static tail that ends the list, as the login capture 1198 has it) and in no KnownAbilities: the client loads only what the records list, and a mount item
/// used without its ability loaded fails with "Ability not loaded" and sends nothing.
static void test_carried_item_abilities(const bs::PlayerSetup& cap, const content::ContentDb& db) {
    std::printf("the abilities of the items the character carries (a mount item's Summon_Mount)\n");
    constexpr int64_t kSummon = 108436548376185ll, kMountA = 9990001, kMountB = 9990002, kNotAMount = 9990003;
    content::MountData data;
    for (const int64_t item : {kMountA, kMountB}) {
        content::MountRow row;
        row.item = item;
        row.cast_ability = kSummon;
        data.mounts.push_back(row);
    }
    content::MountRow no_ability;   // (a row whose data names no ability brings none)
    no_ability.item = 9990004;
    data.mounts.push_back(no_ability);
    const auto item_at = [](svc::ItemContainer c, int slot, int64_t record, unsigned n) {
        svc::InventoryItem it;
        it.container = c;
        it.slot = slot;
        it.item_record_id = record;
        char guid[33];
        std::snprintf(guid, sizeof guid, "%08x%08x%08x%08x", 0xC0000000u + n, 0x1000u + n, 0x2000u + n, n + 1);
        it.instance_guid = guid;
        it.quality = 1;
        it.durability = 11;
        it.created_at_ms = 1'771'299'474'000ll;
        return it;
    };
    std::string error;
    const auto inv = aoc::worldd::items::Inventory::from_items({item_at(svc::ItemContainer::Backpack, 3, kMountA, 1), item_at(svc::ItemContainer::Backpack, 4, kNotAMount, 2),
                                                   item_at(svc::ItemContainer::Backpack, 5, kMountB, 3), item_at(svc::ItemContainer::Backpack, 6, 9990004, 4)},
                                                  error);
    CHECK(inv.has_value(), "the test bag builds");
    if (!inv) return;
    CHECK(bs::carried_item_abilities(&data, *inv) == std::vector<int64_t>{kSummon}, "two mount items and others: Summon_Mount, once");
    CHECK(bs::carried_item_abilities(nullptr, *inv).empty(), "no mount tables: nothing");
    const auto none = aoc::worldd::items::Inventory::from_items({item_at(svc::ItemContainer::Backpack, 3, kNotAMount, 5)}, error);
    CHECK(none && bs::carried_item_abilities(&data, *none).empty(), "no mount item: nothing");
    const auto worn = aoc::worldd::items::Inventory::from_items({item_at(svc::ItemContainer::Equipment, 40, kMountA, 6)}, error);
    CHECK(worn && bs::carried_item_abilities(&data, *worn) == std::vector<int64_t>{kSummon}, "a mount item worn counts too (retail's LoadEquipmentAbilities)");

    // the join's lists
    bs::StatContext ctx = world_stats::of(db);
    svc::Character without = captured_record_with_kit(cap);
    svc::Character with = without;
    with.items.push_back(item_at(svc::ItemContainer::Backpack, 3, kMountA, 7));
    with.items.push_back(item_at(svc::ItemContainer::Backpack, 4, kMountB, 8));
    ctx.mounts = &data;
    const bs::CharacterSetupResult r0 = bs::build_player_setup(without, db, cap.login, session_of(cap), ctx);
    const bs::CharacterSetupResult r1 = bs::build_player_setup(with, db, cap.login, session_of(cap), ctx);
    CHECK(r0.setup && r1.setup, "the characters build");
    if (!r0.setup || !r1.setup) return;
    const auto count_of = [](const std::vector<bs::AbilityRecord>& v, int64_t g) { return std::count_if(v.begin(), v.end(), [&](const bs::AbilityRecord& r) { return r.guid == g; }); };
    CHECK(count_of(r0.setup->records_to_load, kSummon) == 0 && r0.setup->records_to_load.size() == cap.records_to_load.size(), "no mount item: the captured Bard's records");
    CHECK(count_of(r1.setup->records_to_load, kSummon) == 1 && r1.setup->records_to_load.size() == cap.records_to_load.size() + 1, "two mount items: one more record, Summon_Mount");
    CHECK(std::count(r1.setup->known_abilities.begin(), r1.setup->known_abilities.end(), kSummon) == 0 && r1.setup->known_abilities == r0.setup->known_abilities,
          "and KnownAbilities are as they were");
    const std::span<const int64_t> tail = bs::static_known_tail();
    bool tail_last = r1.setup->records_to_load.size() >= tail.size();
    for (size_t i = 0; tail_last && i < tail.size(); ++i) tail_last = r1.setup->records_to_load[r1.setup->records_to_load.size() - tail.size() + i].guid == tail[i];
    CHECK(tail_last, "the static tail still ends the records: the item's ability lies before it");
    bs::StatContext no_mounts = world_stats::of(db);
    const bs::CharacterSetupResult r2 = bs::build_player_setup(with, db, cap.login, session_of(cap), no_mounts);
    CHECK(r2.setup && count_of(r2.setup->records_to_load, kSummon) == 0, "a world with no mount tables adds nothing");
}

/// The setup's stats: the captured Bard in its kit equals the capture, a bare character has the class's attributes, and the other classes
/// have their own stats.
static void test_setup_stats(const bs::PlayerSetup& cap, const content::ContentDb& db, const char* label) {
    std::printf("the stats of the setup (%s)\n", label);
    const bs::StatContext& ctx = world_stats::of(db);
    const bs::CharacterSetupResult r = bs::build_player_setup(captured_record_with_kit(cap), db, cap.login, session_of(cap), ctx);
    CHECK(r.setup.has_value() && r.error.empty(), "the captured Bard in its kit builds a setup");
    if (!r.setup) {
        std::printf("    %s\n", r.error.c_str());
        return;
    }
    const std::vector<std::string> d = diff_setups(*r.setup, cap);
    std::string list;
    for (const auto& x : d) list += (list.empty() ? "" : ", ") + x;
    CHECK(d == std::vector<std::string>{"orgs_at_war_toast"}, "equal to captured_character_setup() in every field but the toast flag, the 442 stats (values, ids, keys, base and equipment) and HeldItems included");
    if (d.size() != 1) std::printf("    differs in: %s\n", list.c_str());
    CHECK(r.setup->stats.owner.items.size() == 335 && r.setup->stats.owner_proxy.items.size() == 96 && r.setup->stats.everyone_proxy.items.size() == 11 &&
              r.setup->stats.float_everyone_proxy.size() == 1,
          "335 Owner, 96 OwnerProxy, 11 EveryoneProxy items and the float array's one");
    CHECK(r.character_stats && r.character_stats->started() && r.character_stats->values().size() == 466 && r.character_stats->faults() == 0 &&
              r.character_stats->unknown_items() == 0,
          "the session starts with the 466 stats of the Bard's four lists (443 replicate: the 442 and TempHealth), no fault and no unknown item");
    // the engine's stats are the setup's: every Owner item is the stat's float
    bool same = r.character_stats.has_value();
    if (same) {
        for (const stats::StatValue& v : r.character_stats->values()) {
            if (v.float_array() || v.replication == 0 || v.replication == 6) continue;
            const bs::StatArray& a = v.replication == 1 ? r.setup->stats.owner : v.replication == 5 ? r.setup->stats.owner_proxy : r.setup->stats.everyone_proxy;
            const auto it = std::find_if(a.items.begin(), a.items.end(), [&](const bs::StatItem& i) { return i.record == v.guid; });
            same = same && it != a.items.end() && it->value == std::bit_cast<uint32_t>(v.value);
        }
    }
    CHECK(same, "every stat of the character is the item of its array");
// Cyb x C | O | A

    // no gear: the six attributes are class_stat_base's
    std::string error;
    svc::Character bare = captured_record(cap);
    const bs::CharacterSetupResult rb = bs::build_player_setup(bare, db, cap.login, session_of(cap), ctx);
    CHECK(rb.setup.has_value(), "the Bard with no items builds");
    if (rb.setup) {
        const auto attrs = db.primary_attributes(content_fixture::kBard, 1, error);
        bool six = attrs.has_value() && attrs->size() == 6;
        if (six) {
            for (const content::PrimaryAttribute& a : *attrs) {
                const auto it = std::find_if(rb.setup->stats.owner.items.begin(), rb.setup->stats.owner.items.end(), [&](const bs::StatItem& i) { return i.record == a.stat_guid; });
                six = six && it != rb.setup->stats.owner.items.end() && std::bit_cast<float>(it->value) == static_cast<float>(a.value);
            }
        }
        CHECK(six, "its six primary attributes are class_stat_base's (Strength 24, Dexterity 24, Constitution 30, Intelligence 39, Wisdom 36, Mentality 30)");
        const std::vector<std::string> dd = diff_setups(*rb.setup, cap);
        bool layout_kept = true, some_values = false;
        const std::pair<const bs::StatArray*, const bs::StatArray*> arrays[] = {{&rb.setup->stats.owner, &cap.stats.owner},
                                                                               {&rb.setup->stats.owner_proxy, &cap.stats.owner_proxy},
                                                                               {&rb.setup->stats.everyone_proxy, &cap.stats.everyone_proxy}};
        for (const auto& [got, want] : arrays) {
            layout_kept = layout_kept && got->array_key == want->array_key && got->items.size() == want->items.size();
            for (size_t i = 0; layout_kept && i < got->items.size(); ++i) layout_kept = got->items[i].id == want->items[i].id && got->items[i].record == want->items[i].record;
        }
        for (const std::string& x : dd) some_values = some_values || x.find("].value") != std::string::npos;
        CHECK(layout_kept && some_values, "with no gear the values differ from the capture (its mitigation, Intelligence and what follows), the ids, keys and order do not");
    }

    // the other classes at level 1 in their own kit: their own stats, not the Bard's
    std::map<int, std::vector<uint32_t>> owner_values;
    for (const gear_fixture::ClassKit& k : gear_fixture::kClassKits) {
        svc::Character c = captured_record(cap);
        c.class_id = k.class_id;
        c.class_name = k.name;
        c.archetype_id = 0;
        c.items = kit_items(k.items, k.count);
        const bs::CharacterSetupResult rc = bs::build_player_setup(c, db, cap.login, session_of(cap), ctx);
        CHECK(rc.setup.has_value() && rc.setup->class_record_guid == k.guid, (std::string(k.name) + ": builds with its class's record Guid").c_str());
        if (!rc.setup) {
            std::printf("    %s\n", rc.error.c_str());
            continue;
        }
        // the classes differ in their own resource stats: the Cleric has one more Owner stat than the Bard, the Mage one fewer
        const size_t owner_stats = rc.setup->stats.owner.items.size();
        CHECK(owner_stats >= 334 && owner_stats <= 336 && rc.setup->stats.owner_proxy.items.size() == 96 && rc.setup->stats.everyone_proxy.items.size() == 11,
              (std::string(k.name) + ": 334 to 336 Owner stats (its own resource stats), 96 OwnerProxy and 11 EveryoneProxy").c_str());
        std::vector<uint32_t> vals;
        for (const bs::StatItem& it : rc.setup->stats.owner.items) vals.push_back(it.value);
        owner_values[k.class_id] = vals;
        CHECK(rc.setup->stats.owner.array_key == 417 && rc.setup->stats.owner_proxy.array_key == 169 && rc.setup->stats.everyone_proxy.array_key == 21,
              (std::string(k.name) + ": the array keys are the captured 417, 169 and 21").c_str());
    }
    int distinct = 0;
    for (const auto& [id, vals] : owner_values) {
        bool unique = true;
        for (const auto& [other, ov] : owner_values) unique = unique && (other == id || ov != vals);
        distinct += unique;
    }
    CHECK(distinct == 8, "no two classes have the same Owner stats");
    const auto find_item = [](const bs::StatArray& a, int64_t guid) { return std::find_if(a.items.begin(), a.items.end(), [&](const bs::StatItem& i) { return i.record == guid; }); };
    const bs::CharacterSetupResult rbard = bs::build_player_setup(captured_record_with_kit(cap), db, cap.login, session_of(cap), ctx);
    svc::Character tank = captured_record(cap);
    tank.class_id = 7;
    tank.class_name = "Tank";
    tank.archetype_id = 0;
    tank.items = kit_items(gear_fixture::kTankKit, static_cast<int>(std::size(gear_fixture::kTankKit)));
    const bs::CharacterSetupResult rtank = bs::build_player_setup(tank, db, cap.login, session_of(cap), ctx);
    constexpr int64_t kBardResource = 6064632920367763592ll;   // the Owner stat the Bard has and the Ranger does not
    CHECK(rbard.setup && rtank.setup && find_item(rbard.setup->stats.owner, kBardResource) != rbard.setup->stats.owner.items.end() &&
              find_item(rtank.setup->stats.owner, kBardResource) == rtank.setup->stats.owner.items.end(),
          "the Bard's own resource stat is in the Bard's Owner array and not in the Tank's");
}

/// The capture's record (the shape of the character: its Guid words, name, race, gender, class, level, customization and spawn) against `content`.
static void check_capture_record(const bs::PlayerSetup& cap, const content::ContentDb& content, const char* label) {
    const svc::Character rec = captured_record_with_kit(cap);
    CHECK(!rec.appearance_json.empty(), (std::string(label) + ": the capture's customization has a JSON form").c_str());
    const bs::CharacterSetupResult r = bs::build_player_setup(rec, content, cap.login, session_of(cap), world_stats::of(content));
    CHECK(r.setup.has_value() && r.error.empty(), (std::string(label) + ": the capture's record builds a setup").c_str());
    if (!r.setup) {
        std::printf("    %s\n", r.error.c_str());
        return;
    }
    const std::vector<std::string> d = diff_setups(*r.setup, cap);
    std::string list;
    for (const auto& x : d) list += (list.empty() ? "" : ", ") + x;
    std::printf("    differs from captured_character_setup() in: %s\n", list.c_str());
    CHECK(d == std::vector<std::string>{"orgs_at_war_toast"},
          (std::string(label) + ": the capture's record wearing the Bard's kit is equal to captured_character_setup() field by field, the 442 stats and HeldItems "
                                "and AppearanceIDs included, but for the toast flag (the record does not carry it)").c_str());
    CHECK(r.notes.size() == 1 && contains(r.notes[0], "13 worn"), (std::string(label) + ": the one note says what the character wears").c_str());
    // the same record with no items: nothing worn, nothing shown
    const bs::CharacterSetupResult none = bs::build_player_setup(captured_record(cap), content, cap.login, session_of(cap), world_stats::of(content));
    CHECK(none.setup && none.setup->held_items.items.empty() && none.setup->appearance_ids.empty() && none.notes.empty(),
          (std::string(label) + ": an unequipped character: no held items, no appearance ids, nothing to note").c_str());
}

static void test_capture_record(const bs::PlayerSetup& cap, const content::ContentDb& content) {
    std::printf("the capture's record shape against the small content.db\n");
    check_capture_record(cap, content, "small content.db");

    const svc::Character rec = captured_record(cap);
    const bs::CharacterSetupResult r = bs::build_player_setup(rec, content, cap.login, session_of(cap), world_stats::of(content));
    if (!r.setup) return;
    CHECK(r.setup->character_guid[0] == 3635609113u && r.setup->name == "RandomChar" && r.setup->race == 0 && r.setup->gender == 0 && r.setup->level == 1 &&
              r.setup->class_record_guid == 108640158570105ll,
          "the Guid words, the name, Kaelar 0, Male 0, level 1 and the Bard's class Guid");
    CHECK(r.setup->x == -777664.5 && r.setup->y == 616578.6875 && r.setup->z == 15952.0498046875 && r.setup->yaw_degrees == 16550.0 * 360.0 / 65536.0,
          "the spawn is the record's position (the capture's spawn point and yaw)");

    // the default spawn is that same point
    svc::Character none = rec;
    none.has_position = false;
    none.position = {1, 2, 3, 4};
    const bs::CharacterSetupResult d = bs::build_player_setup(none, content, cap.login, session_of(cap), world_stats::of(content));
    CHECK(d.setup && d.setup->x == bs::kDefaultSpawn.x && d.setup->y == bs::kDefaultSpawn.y && d.setup->z == bs::kDefaultSpawn.z &&
              d.setup->yaw_degrees == bs::kDefaultSpawn.yaw_degrees,
          "no saved position: the default spawn, whatever the unset position holds");
    CHECK(d.setup && d.setup->x == cap.x && d.setup->y == cap.y && d.setup->z == cap.z && d.setup->yaw_degrees == cap.yaw_degrees,
          "...and the default spawn is the captured character's spawn point and yaw");
    CHECK(d.notes.size() == 1 && contains(d.notes[0], "default spawn"), "...with a note saying so");
}
// -- C-O-A x Cyb --

static void test_cyb_like(const bs::PlayerSetup& cap, const content::ContentDb& content) {
    std::printf("cyb-like records of every race (0-7) and both genders\n");
    struct Case {
        int32_t race_id, gender_id;
        const char *race_name, *gender_name;
        uint8_t race, gender;
    };
    const char* race_names[] = {"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua"};
    const char* gender_names[] = {"Male", "Female"};
    std::vector<Case> cases;
    for (int32_t r = 0; r <= 7; ++r) {
        for (int32_t g = 0; g <= 1; ++g) cases.push_back({r, g, race_names[r], gender_names[g], static_cast<uint8_t>(r), static_cast<uint8_t>(g)});
    }
    for (const Case& c : cases) {
        const std::string label = std::string(c.race_name) + " " + c.gender_name;
        svc::Character rec = cyb_record(c.race_id, c.gender_id, cyb_json_of(c.race_name, c.gender_name));
        rec.has_position = true;
        rec.position = {100.5, -200.25, 300.0, 45.0f};
        bs::LoginIdentity login;
        login.name_option = "cyb";
        login.unique_id = "cyb";
        const bs::CharacterSetupResult r = bs::build_player_setup(rec, content, login, session_of(cap), world_stats::of(content));
        CHECK(r.setup.has_value(), (label + ": builds a setup").c_str());
        if (!r.setup) {
            std::printf("    %s\n", r.error.c_str());
            continue;
        }
        const bs::PlayerSetup& s = *r.setup;
        CHECK(s.race == c.race && s.gender == c.gender && s.customization.race_gender && s.customization.race_gender->race == c.race &&
                  s.customization.race_gender->gender == c.gender,
              (label + ": race " + std::to_string(c.race) + " and gender " + std::to_string(c.gender) + " in the setup and in the customization's Race and Gender bytes").c_str());
        CHECK(s.name == "cyb" && s.level == 1 && s.class_record_guid == content_fixture::kBard && s.x == 100.5 && s.y == -200.25 && s.z == 300.0 &&
                  s.yaw_degrees == 45.0 && s.login.name_option == "cyb" && s.player_id == cap.player_id && s.guids.pawn == cap.guids.pawn,
              (label + ": name, level, class, the saved position, the login and the session").c_str());
        CHECK(s.character_guid[0] == 0xbbec5c86u && s.character_guid[1] == 0xeb101fe5u && s.character_guid[2] == 0xf38c87f0u && s.character_guid[3] == 0xfa22e7d0u,
              (label + ": cyb's id bbec5c86eb101fe5f38c87f0fa22e7d0 as four words").c_str());
        CHECK(codec_round_trip(s.customization), (label + ": the customization encodes and decodes to the last bit").c_str());
        bs::PlayerBootstrap pb(*r.setup, bs::GameStateMode::Tier2);
        const auto seq = pb.ok() ? pb.join_sequence() : std::nullopt;
        CHECK(pb.ok() && seq.has_value() && !seq->empty(), (label + ": a PlayerBootstrap accepts it and builds the join sequence").c_str());
        if (!pb.ok()) std::printf("    %s\n", pb.error().c_str());
        // the pawn's open carries the customization: the second chain of the Pawn role is the open
        bool pawn_open = false;
        if (seq) {
            for (const bs::Send& send : *seq) pawn_open = pawn_open || (send.role == bs::Role::Pawn && send.opens_channel && send.emission.payload.bit_pos() > 26000);
        }
        CHECK(pawn_open, (label + ": the pawn's open is built, over 26,000 bits (the customization alone is about 26,900 in the capture)").c_str());
    }

    // the Bard of cyb at level 2: Strength 26.5 is rounding-ambiguous
    svc::Character l2 = cyb_record(0, 0);
    l2.level = 2;
    const bs::CharacterSetupResult r2 = bs::build_player_setup(l2, content, {}, session_of(cap), world_stats::of(content));
    CHECK(r2.setup.has_value() && r2.setup->level == 2, "level 2 builds");
    CHECK(r2.notes.size() == 1 && contains(r2.notes[0], "default spawn"), "level 2: only the default spawn is noted (the stat program rounds, there is no ambiguity to note)");
    if (r2.setup) {
        const auto it = std::find_if(r2.setup->stats.owner.items.begin(), r2.setup->stats.owner.items.end(),
                                     [](const bs::StatItem& i) { return i.record == content_fixture::kStrength; });
        const float strength = it != r2.setup->stats.owner.items.end() ? std::bit_cast<float>(it->value) : 0;
        CHECK(strength == std::floor(strength) && strength >= 24, "...and the Strength item is a whole number of at least the level-1 Bard's 24");
    }
    // the Tank
    svc::Character tank = cyb_record(0, 0);
    tank.class_id = 7;
    tank.archetype_id = 17754;
    tank.class_name = "Tank";
    const bs::CharacterSetupResult rt = bs::build_player_setup(tank, content, {}, session_of(cap), world_stats::of(content));
    CHECK(rt.setup && rt.setup->class_record_guid == content_fixture::kTank && rt.setup->race == 0, "class 7 is the Tank's class Guid, and a Kaelar Tank is a Kaelar");
    // the class comes from the class id alone: a record with no archetype id and no class name has the same class
    svc::Character bare = cyb_record(0, 0);
    bare.archetype_id = 0;
    bare.class_name = "";
    const bs::CharacterSetupResult rb = bs::build_player_setup(bare, content, {}, session_of(cap), world_stats::of(content));
    CHECK(rb.setup && rb.setup->class_record_guid == content_fixture::kBard && rb.notes.size() == 1 && contains(rb.notes[0], "default spawn"),
          "the class is the class id: no archetype id and no class name needed, and nothing noted");
    // the record's class name and archetype against the class
    svc::Character odd = cyb_record(0, 0);
    odd.class_name = "Mage";
    odd.archetype_id = 17750;
    const bs::CharacterSetupResult ro = bs::build_player_setup(odd, content, {}, session_of(cap), world_stats::of(content));
    CHECK(ro.setup && ro.setup->class_record_guid == content_fixture::kBard && ro.notes.size() >= 2 && contains(ro.notes[0], "Mage") && contains(ro.notes[1], "17750"),
          "a class name and an archetype id that disagree with the class: the class id decides, and both are noted");
    // an unknown JSON key becomes a note
    const bs::CharacterSetupResult rk = bs::build_player_setup(cyb_record(0, 0, replaced(cyb::kAppearanceJson, "\"nailOpacity\":0", "\"nailOpacity\":0,\"hairBand\":3")), content, {}, session_of(cap), world_stats::of(content));
    CHECK(rk.setup && !rk.notes.empty() && contains(rk.notes[0], "hairBand"), "a member of the JSON this server does not read is noted");
}

static const char* race_name_of(int32_t race) {
    static const char* const names[] = {"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua"};
    return names[race];
}

static void test_errors(const bs::PlayerSetup& cap, const content::ContentDb& content) {
    std::printf("build_player_setup: what is refused\n");
    const auto build = [&](const svc::Character& c) { return bs::build_player_setup(c, content, {}, session_of(cap), world_stats::of(content)); };
    const auto refused = [&](const svc::Character& c, const char* part, const char* msg) {
        const bs::CharacterSetupResult r = build(c);
        const bool ok = !r.setup && contains(r.error, part);
        CHECK(ok, msg);
        if (!ok) std::printf("    error: '%s'\n", r.error.c_str());
    };
    const svc::Character base = cyb_record(0, 0);
    CHECK(build(base).setup.has_value(), "the base record builds");

    svc::Character c = base;
    c.id = "bbec5c86eb101fe5f38c87f0fa22e7d";
    refused(c, "32 hex digits", "a 31-digit id");
    c = base;
    c.id = "";
    refused(c, "32 hex digits", "an empty id");
    c = base;
    c.name = "";
    refused(c, "no name", "an empty name");
    c = base;
    c.name = std::string("a\0b", 3);
    refused(c, "name cannot be sent", "a name with a NUL");
    c = base;
    c.name = "\xC3\x28";
    refused(c, "name cannot be sent", "a name that is not UTF-8");
// ~ C.O.A & Cyb ~

    for (int32_t race : {8, 9, 10, -1, 100}) {
        c = base;
        c.race_id = race;
        const bs::CharacterSetupResult r = build(c);
        const bool ok = !r.setup && contains(r.error, ("race id " + std::to_string(race)).c_str()) && contains(r.error, "not a playable race") &&
                        r.refusal == bs::SetupRefusal::UnsupportedRace;
        CHECK(ok, ("race id " + std::to_string(race) + " (Tulnar or past the enum) is an unsupported race naming the id").c_str());
    }
    for (int32_t race = 0; race <= 7; ++race) {
        c = base;
        c.race_id = race;
        c.appearance_json = cyb_json_of(race_name_of(race), "Male");
        CHECK(build(c).setup.has_value(), ("race id " + std::to_string(race) + " is accepted").c_str());
    }
    c = base;
    c.gender_id = 2;
    refused(c, "gender id 2", "gender 2");
    c.gender_id = -1;
    refused(c, "gender id -1", "gender -1");
    c.gender_id = 3;
    refused(c, "gender id 3", "gender 3");
    c = base;
    c.gender_id = 0;
    CHECK(build(c).setup.has_value(), "gender 0 is Male");

    c = base;
    c.level = 0;
    refused(c, "level 0", "level 0");
    c.level = 51;
    refused(c, "level 51", "level 51");
    c.level = 3;
    CHECK(build(c).setup.has_value(), "level 3 builds: the stat engine computes every level of content.db's range, class_stat_base is not read");

    c = base;
    c.class_id = 0;
    refused(c, "class id 0", "a record with no class (the service could not derive it)");
    CHECK(contains(build(c).error, "could not derive"), "...which says why");
    c.class_id = 9;
    refused(c, "class id 9", "a class id past the enum");
    c.class_id = -1;
    refused(c, "class id -1", "a negative class id");
    c.class_id = 8;
    CHECK(build(c).setup.has_value(), "the Mage builds: the small content.db has a row for every class");
    c.class_id = 3;
    CHECK(build(c).setup.has_value(), "the Cleric builds: its stats come from the stat program, not from class_stat_base");
    c = base;
    c.archetype_id = 0;
    CHECK(build(c).setup.has_value(), "a record with no archetype id is fine: the class id decides");

    c = base;
    c.appearance_json = "";
    refused(c, "not valid JSON", "an empty appearance");
    c.appearance_json = "{\"presetGuid\":\"5\"}";
    refused(c, "presetGuid", "an appearance naming a preset");
    c = cyb_record(0, 0, cyb_json_of("Dunir", "Male"));
    refused(c, "Dunir", "a Dunir appearance on a Kaelar record");
    c = cyb_record(0, 1);
    refused(c, "Female", "a Male appearance on a Female record");

    c = base;
    c.has_position = true;
    c.position = {std::numeric_limits<double>::quiet_NaN(), 0, 0, 0};
    refused(c, "not finite", "a saved position with NaN");
    c.position = {0, 0, 0, std::numeric_limits<float>::infinity()};
    refused(c, "not finite", "a saved yaw of infinity");
    c.position = {0, 0, 0, 0};
    CHECK(build(c).setup.has_value(), "a saved position at the origin is a position, not a missing one");

    // a stat run that faults (the fixture's shirt gives Constitution +1e39, which no float32 holds) is no character's stats: the join refuses it
    c = base;
    svc::InventoryItem shirt;
    shirt.container = svc::ItemContainer::Equipment;
    shirt.slot = 19;
    shirt.item_record_id = content_fixture::item::kShirt;
    shirt.instance_guid = "0123456789abcdef0123456789abcdef";
    shirt.quantity = 1;
    shirt.quality = 1;
    shirt.durability = 11;
    shirt.created_at_ms = 1'771'299'474'000ll;
    c.items.push_back(shirt);
    {
        const bs::CharacterSetupResult rf = build(c);
        CHECK(!rf.setup && contains(rf.error, "stat program faulted"), "a worn item that makes the stat program fault is refused at the join, naming the fault");
        c.items.clear();
        CHECK(build(c).setup.has_value(), "(the same character without it builds)");
    }

    // refusals leave the result empty
    c = base;
    c.race_id = 8;
    const bs::CharacterSetupResult r = build(c);
    CHECK(!r.setup && !r.error.empty(), "a refusal has an error and no setup");
}

/// With the real file: every case of the corpus made from items (the captured Bard, the retail Ranger, the 16 kit cases) is rebuilt from
/// its items by the C++ gear rules and the real tables, and gives Go's mods, tags and digest of every stat.
static void test_corpus_cases(const content::ContentDb& db) {
    std::printf("the corpus's gear cases against the real tables (Go's mods, tags and digest of every stat)\n");
    const bs::StatContext& ctx = world_stats::of(db);
    std::string error;
    const auto slot_names = db.equip_slot_names(error);
    CHECK(slot_names.has_value() && slot_names->size() == 83, "the 83 EEquipSlot names are read");
    if (!slot_names) return;
    std::map<std::string, int> slot_of;
    for (const auto& [slot, name] : *slot_names) slot_of[name] = slot;
    size_t cases = 0, good = 0;
    for (const stats_fixture::CorpusCase& c : stats_fixture::corpus().cases) {
        if (!c.has_gear) continue;
        ++cases;
        std::vector<stats::WornItem> worn;
        bool named = true;
        for (const stats_fixture::CorpusItem& it : c.items) {
            int slot = -1;
            if (slot_of.count(it.slot)) slot = slot_of[it.slot];
            else if (it.slot.rfind("Slot", 0) == 0) slot = std::atoi(it.slot.c_str() + 4);
            named = named && slot >= 0;
            worn.push_back({it.record, it.quality, slot});
        }
        const stats::GearInputs g = stats::gear_inputs(*ctx.gear, worn, c.half_up ? stats::Rounding::HalfUp : stats::Rounding::None);
        std::string why = named ? std::string() : "a slot with no number";
        if (why.empty()) why = mods_difference(g.mods, c);
        if (why.empty()) {
            // the capture cases give the slots as Slot<N>: the tags are compared by the slot's number
            std::vector<std::pair<std::string, std::string>> a, b = c.tags;
            const auto canon = [&](const std::string& s) { return s.rfind("Slot", 0) == 0 ? (slot_names->count(std::atoi(s.c_str() + 4)) ? slot_names->at(std::atoi(s.c_str() + 4)) : s) : s; };
            for (const auto& t : g.equipped) a.emplace_back(canon(t.slot), t.tag);
            for (auto& t : b) t.first = canon(t.first);
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a != b) why = "the tags differ";
        }
        if (why.empty() && !matches_corpus_digest(ctx, c, g, why)) why = "digest: " + why;
        if (!why.empty()) std::printf("    %s: %s\n", c.name.c_str(), why.c_str());
        good += why.empty();
    }
    CHECK(cases == 18 && good == cases, "the captured Bard, the retail Ranger and the sixteen kit cases: mods, tags and every stat equal Go's (bit for bit)");
}
// (C.O.A) (C_Y_B)

static void test_real_content(const bs::PlayerSetup& cap) {
    std::printf("against the real content.db (AOC_CONTENT_DB)\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (!env || !*env) {
        std::printf("  [skip] AOC_CONTENT_DB is not set (cd services && go run ./cmd/contentc; AOC_CONTENT_DB=dist\\content\\content.db)\n");
        return;
    }
    std::string error;
    auto db = content::ContentDb::open(env, error);
    CHECK(db.has_value(), "the real content.db opens");
    if (!db) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    check_capture_record(cap, *db, "real content.db");
    test_setup_stats(cap, *db, "the real content.db");
    test_kits_against_go(*db);
    test_corpus_cases(*db);

    const bs::CharacterSetupResult r = bs::build_player_setup(cyb_record(0, 0), *db, cap.login, session_of(cap), world_stats::of(*db));
    CHECK(r.setup.has_value(), "cyb builds against the real content.db");
    if (!r.setup) {
        std::printf("    %s\n", r.error.c_str());
        return;
    }
    std::map<int64_t, uint32_t> v;
    for (const auto& it : r.setup->stats.owner.items) v[it.record] = it.value;
    const auto f = [&](int64_t guid) { return std::bit_cast<float>(v[guid]); };
    CHECK(f(109183598614138ll) == 24.0f && f(109183598614139ll) == 24.0f && f(109183598614140ll) == 30.0f && f(109183598614143ll) == 30.0f && f(109183598614142ll) == 36.0f &&
              f(109183598614141ll) == 39.0f,
          "cyb's Owner stats: Strength 24, Dexterity 24, Constitution 30, Mentality 30, Wisdom 36, Intelligence 39 (the capture has 60)");
    CHECK(r.setup->class_record_guid == 108640158570105ll, "cyb's class is the Bard's record Guid");

    // every class builds at levels 1 and 50
    bool all = true;
    for (int32_t class_id = 1; class_id <= 8; ++class_id) {
        for (int level : {1, 20, 50}) {
            svc::Character c = cyb_record(0, 0);
            c.class_id = class_id;
            c.archetype_id = 0;
            c.class_name = "";
            c.level = level;
            all = all && bs::build_player_setup(c, *db, {}, session_of(cap), world_stats::of(*db)).setup.has_value();
        }
    }
    CHECK(all, "all 8 classes (ECharacterClass 1-8) build at levels 1, 20 and 50");
    // every race, both genders and every class join: the old table let only a Bard, a Tank and a Mage through
    bool every = true;
    for (int32_t race = 0; race <= 7; ++race) {
        for (int32_t gender = 0; gender <= 1; ++gender) {
            for (int32_t class_id = 1; class_id <= 8; ++class_id) {
                svc::Character c = cyb_record(race, gender, cyb_json_of(race_name_of(race), gender ? "Female" : "Male"));
                c.class_id = class_id;
                c.class_name = bs::client_class_name(class_id);
                c.archetype_id = 0;
                const bs::CharacterSetupResult rr = bs::build_player_setup(c, *db, {}, session_of(cap), world_stats::of(*db));
                const bool ok = rr.setup && rr.setup->race == race && rr.setup->gender == gender;
                if (!ok) std::printf("    race %d gender %d class %d: %s\n", race, gender, class_id, rr.error.c_str());
                every = every && ok;
            }
        }
    }
    CHECK(every, "all 8 races x 2 genders x 8 classes build a setup with that race and gender");
    svc::Character l20 = cyb_record(0, 0);
    l20.level = 20;
    const bs::CharacterSetupResult r20 = bs::build_player_setup(l20, *db, {}, session_of(cap), world_stats::of(*db));
    CHECK(r20.setup.has_value(), "the Bard at level 20 builds");
}
// [C Y B ~ C-O-A]

int main() {
    const auto cap = bs::captured_character_setup();
    if (!cap) {
        std::printf("  [FAIL] captured_character_setup() did not read\n");
        return 1;
    }
    test_tables();
    test_capture_roundtrip(*cap);
    test_cyb_customization();
    test_customization_errors();
    test_customization_corpus();
    test_stats_setup_for();

    const auto dir = content_fixture::make_temp_dir("aoc_worldd_character_setup_test_");
    if (content_fixture::make_db(dir / "content.db")) {
        std::string error;
        auto db = content::ContentDb::open((dir / "content.db").string(), error);
        if (db) {
            test_capture_record(*cap, *db);
            test_setup_stats(*cap, *db, "the small content.db");
            test_carried_item_abilities(*cap, *db);
            test_kits_against_go(*db);
            test_cyb_like(*cap, *db);
            test_errors(*cap, *db);
        } else {
            std::printf("  [FAIL] the small content.db did not open: %s\n", error.c_str());
            g_fail++;
        }
    } else {
        g_fail++;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);

    test_real_content(*cap);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

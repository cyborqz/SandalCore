// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_emote_list_tests.inc: the emote commands of the game client (content/emote_commands.h)
//
//  The game's emote list is 54 command words for 48 abilities.  A word is matched without regard to case; /sit plays General_Rest and /wave plays Emote_Wave, the same ability as /hi.
//  The ability of a word is looked up in the content tables the server runs from: its name agrees with the list's, and the words whose ability the tables do not hold are counted.
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================
#include "content/emote_commands.h"

#include <cctype>

namespace emote_list {

namespace ec = aoc::worldd::content;

static std::string upper_case(std::string_view word) {
    std::string out(word);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

/// Every second letter in capitals, the first one included: "wave" is "WaVe".
static std::string mixed_case(std::string_view word) {
    std::string out(word);
    for (size_t i = 0; i < out.size(); i += 2) out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    return out;
}

}  // namespace emote_list

static void test_emote_list_is_the_games_54_words() {
    std::printf("the emote list: the game's 54 command words for 48 abilities\n");
    namespace ec = emote_list::ec;
    CHECKF(ec::kEmoteCommandCount == 54, "the list holds 54 command words (%zu)", ec::kEmoteCommandCount);
    std::set<std::string> words;
    std::set<int64_t> abilities;
    bool lower = true, plain = true, named = true;
    for (const ec::EmoteCommand& e : ec::kEmoteCommands) {
        words.insert(std::string(e.command));
        abilities.insert(e.ability);
        named = named && !e.name.empty() && e.ability != 0;
        for (const char c : e.command) {
            lower = lower && !(c >= 'A' && c <= 'Z');
            plain = plain && c > 32 && c < 127;
        }
    }
    CHECKF(words.size() == 54, "the 54 words are all different (%zu different)", words.size());
    CHECK(lower && plain && named, "every word is lower case, printable ASCII with no space, and has an ability and its name");
    CHECKF(abilities.size() == 48, "the words play 48 different abilities (%zu)", abilities.size());
    // the words that share an ability: the three laughs, hi and wave, the two taunts, the two thanks, the two scares
    const ec::EmoteCommand* laugh = ec::find_emote_command("laugh");
    const ec::EmoteCommand* haha = ec::find_emote_command("haha");
    const ec::EmoteCommand* lol = ec::find_emote_command("lol");
    const ec::EmoteCommand* thanks = ec::find_emote_command("thanks");
    const ec::EmoteCommand* ty = ec::find_emote_command("ty");
    const ec::EmoteCommand* scared = ec::find_emote_command("scared");
    const ec::EmoteCommand* cower = ec::find_emote_command("cower");
    const ec::EmoteCommand* neener = ec::find_emote_command("neener");
    const ec::EmoteCommand* taunt = ec::find_emote_command("taunt");
    CHECK(laugh && haha && lol && laugh->ability == haha->ability && haha->ability == lol->ability, "laugh, haha and lol play one ability");
    CHECK(thanks && ty && thanks->ability == ty->ability && scared && cower && scared->ability == cower->ability && neener && taunt && neener->ability == taunt->ability,
          "thanks and ty, scared and cower, neener and taunt share one ability each");
}

AOC_REGISTER_TEST(test_emote_list_is_the_games_54_words);

static void test_emote_words_are_matched_without_regard_to_case() {
    std::printf("the emote list: a word is matched in any case, and nothing but a word on the list matches\n");
    namespace ec = emote_list::ec;
    size_t found = 0, wrong = 0;
    for (const ec::EmoteCommand& e : ec::kEmoteCommands) {
        for (const std::string& form : {std::string(e.command), emote_list::upper_case(e.command), emote_list::mixed_case(e.command)}) {
            const ec::EmoteCommand* f = ec::find_emote_command(form);
            if (f && f->ability == e.ability && f->command == e.command) ++found;
            else ++wrong;
        }
    }
    CHECKF(found == 54 * 3 && wrong == 0, "each of the 54 words is found as listed, in capitals and in mixed case (%zu of 162, %zu wrong)", found, wrong);
    // the three words the game itself writes with a capital
    CHECK(ec::find_emote_command("Bow") && ec::find_emote_command("Cower") && ec::find_emote_command("WipeBrow") && ec::find_emote_command("WIPEBROW") && ec::find_emote_command("wipebrow"),
          "Bow, Cower and WipeBrow, as the game writes them, are found like the rest");
    // the owner's picks
    const ec::EmoteCommand* wave = ec::find_emote_command("WaVe");
    const ec::EmoteCommand* hi = ec::find_emote_command("HI");
    const ec::EmoteCommand* sit = ec::find_emote_command("Sit");
    CHECK(wave && wave->ability == 107140329395832ll && std::string(wave->name) == "Emote_Wave", "/wave plays Emote_Wave");
    CHECK(hi && wave && hi->ability == wave->ability, "/hi plays the same ability");
    CHECK(sit && sit->ability == aoc::worldd::content::kGuidRest && std::string(sit->name) == "General_Rest", "/sit plays General_Rest");
    // what is no emote: a prefix, a longer word, a word with something around it, the ability's name, a command that is none, an empty word, other bytes
    size_t matched = 0;
    for (const std::string_view w : {std::string_view(""), std::string_view(" "), std::string_view("wav"), std::string_view("waves"), std::string_view("wave "), std::string_view(" wave"),
                                      std::string_view("/wave"), std::string_view("wave2"), std::string_view("Emote_Wave"), std::string_view("emote_wave"), std::string_view("General_Rest"),
                                      std::string_view("si t"), std::string_view("s"), std::string_view("sitt"), std::string_view("wipe brow"), std::string_view("dancing"),
                                      std::string_view("tell"), std::string_view("say"), std::string_view("wa\0ve", 5), std::string_view("wave\0", 5), std::string_view("\xC3\x9Cwave"),
                                      std::string_view("WA\xC3\x9CVE")}) {
        if (ec::find_emote_command(w)) ++matched;
    }
    CHECKF(matched == 0, "22 things that are no emote find nothing: an empty word, a prefix, a longer word, a word with a space, a slash or a NUL around it, an ability's name, other bytes (%zu found)", matched);
    CHECK(ec::find_emote_command(std::string(5000, 'a')) == nullptr, "a word of 5,000 characters finds nothing");
    CHECK(ec::is_emote_ability(107140329395832ll) && ec::is_emote_ability(aoc::worldd::content::kGuidRest) && !ec::is_emote_ability(aoc::worldd::content::kGuidSprint) &&
              !ec::is_emote_ability(aoc::worldd::content::kGuidDodge) && !ec::is_emote_ability(0),
          "Emote_Wave and General_Rest are abilities an emote word plays; Sprint, Dodge and 0 are not");
}

AOC_REGISTER_TEST(test_emote_words_are_matched_without_regard_to_case);

static void test_emote_list_agrees_with_the_content_tables() {
    std::printf("the emote list against the content tables the server runs from\n");
    if (!ability_rig::real_db_present()) {
        std::printf("  (no content.db at %s: skipped)\n", ability_rig::real_db_path().c_str());
        return;
    }
    namespace ec = emote_list::ec;
    Fixture fx(/*use_real=*/true);
    const abilities::Catalog* cat = fx.world->ability_catalog();
    CHECK(cat != nullptr, "the world holds the ability tables");
    if (!cat) return;
    size_t in_tables = 0, without = 0;
    bool names_agree = true, tagged = true;
    std::string missing;
    for (const ec::EmoteCommand& e : ec::kEmoteCommands) {
        const abilities::AbilityDef* d = cat->ability(e.ability);
        if (!d) {
            ++without;
            missing += (missing.empty() ? "" : ", ") + std::string(e.command);
            continue;
        }
        ++in_tables;
        names_agree = names_agree && d->row.name == e.name;
        const bool emote_tag = std::find(d->row.tags.begin(), d->row.tags.end(), "emote") != d->row.tags.end();
        tagged = tagged && (emote_tag || e.ability == aoc::worldd::content::kGuidRest);
    }
    CHECKF(in_tables + without == 54, "every word has an ability row in the content tables or is counted without one (%zu and %zu)", in_tables, without);
    CHECK(names_agree, "where the tables hold the ability, its name is the list's (the list was copied from the same data)");
    CHECK(tagged, "and it carries the tag emote (General_Rest, the Rest, has none)");
    std::printf("  (%zu words play an ability the content tables hold; %zu do not: %s)\n", in_tables, without, missing.c_str());
    CHECK(cat->ability(107140329395832ll) != nullptr && cat->ability(aoc::worldd::content::kGuidRest) != nullptr, "Emote_Wave has a row in the tables, and so does General_Rest");
}

AOC_REGISTER_TEST(test_emote_list_agrees_with_the_content_tables);

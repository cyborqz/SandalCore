// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  content/emote_commands.h: the emote commands of the game client (/sit, /wave, /dance ...) and the ability each one plays
//
//  The client sends a typed emote command to the server as its bare word (ServerPlayEmote, "sit" for /sit, no slash) and the server plays the ability the word stands for.  The game keeps
//  the list in its design data (the record EmotesLookup): 54 command words for 48 abilities.  Several words can play one ability (hi and wave, laugh, haha and lol), and the list holds
//  /sit, which plays General_Rest, the ability of the Rest.  The words are copied here as the game has them, in lower case; a word the player types is matched without regard to case
//  (the game's list has Bow, Cower and WipeBrow with capitals).  A word that is not on the list is no emote.
//
//  The list is data of the game, not a choice of ours; when the design data changes, this table changes with it (tools/ has no generator: the rows are the record's, one for one).
//  Header only.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace aoc::worldd::content {

/// One word of the game's emote list.
struct EmoteCommand {
    std::string_view command;   // the word, lower case
    int64_t          ability;   // the ability record it plays
    std::string_view name;      // that record's name, for the log
};

inline constexpr EmoteCommand kEmoteCommands[] = {
    {"airguitar", 6064634769590583305ll, "Emote_AirGuitar"},
    {"bigcelebrate", 6064630553138364425ll, "Emote_BigCelebrate"},
    {"bigwave", 6064629698265350182ll, "Emote_BigWave"},
    {"bow", 6064629698313060398ll, "Emote_Bow"},
    {"bye", 6064629693810410926ll, "Emote_Bye"},
    {"charge", 6064629693688317136ll, "Emote_Charge"},
    {"cheer", 6064629698299822123ll, "Emote_Cheer"},
    {"chicken", 6064629698279440423ll, "Emote_Chicken"},
    {"clap", 6064629698284879912ll, "Emote_Clap"},
    {"cower", 6064629693813228976ll, "Emote_Scared"},
    {"creepy", 6064629698305458221ll, "Emote_Creepy"},
    {"crossarms", 6064634769636261899ll, "Emote_CrossArms"},
    {"dance", 6064629698452980380ll, "Emote_Dance"},
    {"dance1", 6064632512507222983ll, "Emote_Dance1"},
    {"dance2", 6064632512509582280ll, "Emote_Dance2"},
    {"dance3", 6064632512511220681ll, "Emote_Dance3"},
    {"dance4", 6064632512512465866ll, "Emote_Dance4"},
    {"excited", 6064629698317713456ll, "Emote_Excited"},
    {"facepalm", 6064634769645371404ll, "Emote_FacePalm"},
    {"fistpump", 6064629698301526060ll, "Emote_FistPump"},
    {"flex", 6064629698334228531ll, "Emote_Flex"},
    {"frustrated", 6064634769733124109ll, "Emote_Frustrated"},
    {"golfclap", 6064629698285928489ll, "Emote_GolfClap"},
    {"groove", 6064634769756192782ll, "Emote_Groove"},
    {"haha", 6064629693708830004ll, "Emote_Laugh"},
    {"hi", 107140329395832ll, "Emote_Wave"},
    {"kiss", 6064634769763336207ll, "Emote_Kiss"},
    {"kneel", 6064629732275979608ll, "Emote_Kneel"},
    {"laugh", 6064629693708830004ll, "Emote_Laugh"},
    {"lean", 6064632598368550924ll, "Emote_Lean"},
    {"lmao", 6064629693720364355ll, "Emote_Laugh2"},
    {"lol", 6064629693708830004ll, "Emote_Laugh"},
    {"looking", 6064634769777295376ll, "Emote_Looking"},
    {"luckydance", 6064632598369665037ll, "Emote_Dance_Lucky"},
    {"maybe", 6064634769787715601ll, "Emote_Maybe"},
    {"neener", 6064629693811787183ll, "Emote_Neener"},
    {"no", 6064629698298249258ll, "Emote_No"},
    {"nod", 6064629693808772525ll, "Emote_Nod"},
    {"notrouble", 6064634769793089554ll, "Emote_NoTrouble"},
    {"point", 6064629693040296490ll, "Emote_Point"},
    {"salute", 6064629698332327986ll, "Emote_Salute"},
    {"scared", 6064629693813228976ll, "Emote_Scared"},
    {"searching", 6064634769803444243ll, "Emote_Searching"},
    {"shakefist", 6064634769810456596ll, "Emote_ShakeFist"},
    {"shrug", 6064629698330296369ll, "Emote_Shrug"},
    {"sit", 109393746155128ll, "General_Rest"},
    {"stinks", 6064634769818845205ll, "Emote_Stinks"},
    {"surrender", 6064634769830576150ll, "Emote_Surrender"},
    {"taunt", 6064629693811787183ll, "Emote_Neener"},
    {"thanks", 6064629698314240047ll, "Emote_Thanks"},
    {"ty", 6064629698314240047ll, "Emote_Thanks"},
    {"wave", 107140329395832ll, "Emote_Wave"},
    {"wipebrow", 6064634769836867607ll, "Emote_WipeBrow"},
    {"yes", 6064629693806740908ll, "Emote_Yes"},
};

inline constexpr size_t kEmoteCommandCount = std::size(kEmoteCommands);

namespace emote_detail {
/// ASCII lower case of one character (the commands are ASCII; any other byte stays as it is and never matches a word of the list).
constexpr char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool same_word(std::string_view typed, std::string_view word) {
    if (typed.size() != word.size()) return false;
    for (size_t i = 0; i < typed.size(); ++i) {
        if (lower(typed[i]) != word[i]) return false;
    }
    return true;
}
}  // namespace emote_detail

/// The entry of the word `typed` (any case, no slash, nothing around it), or null when it is not on the list.
inline const EmoteCommand* find_emote_command(std::string_view typed) {
    for (const EmoteCommand& e : kEmoteCommands) {
        if (emote_detail::same_word(typed, e.command)) return &e;
    }
    return nullptr;
}

/// Whether `ability` is one an emote word plays (General_Rest among them).
inline bool is_emote_ability(int64_t ability) {
    for (const EmoteCommand& e : kEmoteCommands) {
        if (e.ability == ability) return true;
    }
    return false;
}

}  // namespace aoc::worldd::content

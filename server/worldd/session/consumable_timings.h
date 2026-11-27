// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/consumable_timings.h
//
//  When the montage of a consumable's ability is cleared (the ability item is replaced by an empty one), counted from the use.  The four montage assets are shared by
//  all the consumables of a kind, so the table is by asset, not by ability.  A scroll's montage is cleared after about a second, an elixir's (the potion montage) after
//  about 1.9 s; a meal's or a drink's montage has no time here: the character stays seated after eating, until they walk or use something else, and only then
//  does it jump to its end (consumable_stand_position) and, when it reaches that end, lose its montage (consumable_montage_length).
//  Numbers measured on the retail recordings of three scrolls (0.94, 0.98 and 1.22 s) and an elixir (1.86 s).
// ============================================================================
#pragma once

#include <string>

namespace aoc::worldd {

/// Seconds after the use at which the montage of the asset at `soft_path` is cleared; -1 for a montage that has no fixed end.
inline double consumable_montage_end(const std::string& soft_path) {
    if (soft_path.find("Consumable_Scroll") != std::string::npos) return 0.98;
    if (soft_path.find("Consumable_Potion") != std::string::npos) return 1.86;
    return -1;
}

/// Seconds after the use at which the effects of the asset's kind may first apply — the least time its eating or drinking bar needs.  The bar's length is the
/// montage's own (a recorded meal asks for its apply hit 6.95 s after the use); the ability record carries no cast time.  A use of such a kind holds its timed
/// effects (the buff) back until the apply hit the client asks for when its bar completes, and that hit lands no earlier than this.  The hit does
/// not end the use: the game leaves the character seated, with its montage and Resting on, until they walk or use something else.  The instant kinds (a scroll's,
/// a potion's, a salve's, an elixir's) keep the cast's own step, as their client asks for their hit right after the use.  -1: the instant kinds.
inline double consumable_effects_delay(const std::string& soft_path) {
    if (soft_path.find("Consumable_Food") != std::string::npos) return 2.0;
    if (soft_path.find("Consumable_Regen") != std::string::npos) return 2.0;
    return -1;
}

/// When the montage of an eating asset gives the character one of the ability's effects, by the effect's key in the ability's EffectsIds: an effect notify of the
/// montage names the key and fires when the montage reaches it.  The Regen asset (rations, and the food and the drink of their kind) has two: key 1, the use's own
/// 20 s effect, at 3.0077 s of montage time, and key 3, Resting, at 3.01 s.  The Food asset (meals and drinks) has one: key 3, Resting, at 3.01 s; its buff comes with its
/// apply hit and its key 1 (the Nourishment) with neither, so a meal never gets it.  A montage that starts at its first section reaches 3.0 s after 2.0 s (that section
/// runs to 2.0 s and goes on at SitStart, 3.0 s); one that starts at PreSit (2.0 s), the row the client takes while the character already has Resting, after 1.0 s.  The
/// answer is the seconds after the use; -1: no notify of the asset gives that key.
inline double consumable_effect_notify(const std::string& soft_path, const std::string& start_section, int key) {
    const double reached = start_section == "PreSit" ? 1.0 : 2.0;
    if (soft_path.find("Consumable_Regen") != std::string::npos) {
        if (key == 1) return reached + 0.0077105;
        if (key == 3) return reached + 0.01;
    } else if (soft_path.find("Consumable_Food") != std::string::npos) {
        if (key == 3) return reached + 0.01;
    }
    return -1;
}

/// Where the montage of the asset at `soft_path` is told to jump when the character stands up from eating or drinking: the start of its last section, the one
/// the eating ability's own branch for walking or taking damage names (as the Rest's montage is jumped to its last section).  A meal's and a drink's is the
/// Food asset's, a ration's the Regen asset's, which has the same sections laid out later.  -1: the asset is not an eating one.
inline float consumable_stand_position(const std::string& soft_path) {
    if (soft_path.find("Consumable_Food") != std::string::npos) return 17.890932f;
    if (soft_path.find("Consumable_Regen") != std::string::npos) return 28.802214f;
    return -1.0f;
}

/// How long the montage of the asset at `soft_path` is, in seconds.  What plays after the jump to its last section (consumable_stand_position) is the rest of it, so the montage
/// ends 1.044 s after the jump for the Food asset (18.93513 s long, jumped to 17.890932 s) and 1.173 s after it for the Regen asset (29.97513 s long, jumped to 28.802214 s); the
/// item of the use loses its montage then.  -1: the asset is not an eating one.
inline double consumable_montage_length(const std::string& soft_path) {
    if (soft_path.find("Consumable_Food") != std::string::npos) return 18.93513;
    if (soft_path.find("Consumable_Regen") != std::string::npos) return 29.97513;
    return -1.0;
}

}  // namespace aoc::worldd

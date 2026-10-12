// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// The listed differences of test_worldd_tooltips.cpp (included inside its anonymous namespace): the ability (or "*" for every ability), the problem's
// text as the test words it (a prefix), why.  Exact: a problem that is not here fails the test.  A difference a fix of the runtime removes leaves its
// line here harmlessly; the reasons are the R_ constants of the test.
const Known kKnown[] = {
    {"*", "RANGE:tooltip", R_RANGE},
    {"*", "CAST:montage", R_MONTAGE},
    {"*", "HITREF:", R_REF},
    {"*", "APPLYREF:", R_REF},
    {"*", "EFFDUR?:", R_DURSTAT},
    {"*", "EFFDUR!:", R_DISABLE},
    // costs only the stat engine or the cast in progress gives (the key states no value; the runtime's is printed)
    {"Cleric_Barrier", "COST:Health the generator could not evaluate it", R_STATS},
    {"Cleric_DivineInfusion", "COST:Cleric_DivinePower the generator could not evaluate it", R_PROGRESS},
    {"Fighter_FormOfCelerity", "COST:Fighter_Momentum the generator could not evaluate it", R_STATS},
    {"Fighter_FormOfFerocity", "COST:Fighter_Momentum the generator could not evaluate it", R_STATS},
    {"Fighter_FormOfFluidity", "COST:Fighter_Momentum the generator could not evaluate it", R_STATS},
    {"Fighter_Wallop", "COST:Fighter_Momentum the generator could not evaluate it", R_STATS},
    {"Tank_Fortify", "COST:Tank_Courage the generator could not evaluate it", R_STATS},
    {"General_Sprint", "COST:Stamina the generator could not evaluate it", R_PROGRESS},
    // hit numbers only the stat engine or a hit in progress gives
    {"Rogue_Throw", "HIT:Rogue_Throw needs the player's stats", R_STATS},
    {"Summoner_Pet_Hunter_Eviscerate", "HIT:Summoner_Pet_Hunter_Eviscerate needs the player's stats", R_STATS},
    {"Bard_Misc_Misdirection", "HIT:Bard_Misdirection_Dummy has no stat mod", R_DUMMY},
    // a duration that is a bare GetTerm
    {"Ranger_HuntOfTheBear", "EFFDUR:Ranger_MarkOfTheBear neither", R_TERM},
    {"Ranger_HuntOfTheRaven", "EFFDUR:Ranger_MarkOfTheRaven neither", R_TERM},
    {"Ranger_HuntOfTheTiger", "EFFDUR:Ranger_MarkOfTheTiger neither", R_TERM},
    // activation requirements the stat program cannot run
    {"Ranger_Dash", "FORMULA:its activation requirement", R_HOSTFN},
    {"Summoner_PetCommand_Sic", "FORMULA:its activation requirement", R_HOSTFN},
    {"Economy_FishingCast", "FORMULA:its activation requirement", R_HOSTFN},
    {"Economy_SportFishingCast", "FORMULA:its activation requirement", R_HOSTFN},
};

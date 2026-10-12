// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/character_setup.h
//
//  Plan E1: the player's real values.  Builds the PlayerSetup of a joining player
//  from what the platform knows of it: the character record (svc::Character, from
//  the character service), content.db (the class and the six primary attributes),
//  the login identity and the session's values.  Pure: no sockets, no database but
//  content.db read-only.  A failure comes back as a message and no setup (the login
//  is refused); nothing is guessed, so a value that cannot be proven is an error.
//
//  Where each part of the PlayerSetup comes from (transcript section 4.1 maps the
//  record field by field):
//
//    character_guid   the record's id, 32 hex digits, as four big-endian words
//                     (capture: D8B2FE19 95FB4DF9 9B1E3958 6C2B2BD4 = 3635609113, ...)
//    name, level      the record's, as is
//    race, gender     the record holds the client's own enums (ECharacterRace, ECharacterGender), which are
//                     what the wire carries: taken as they are (plan B1)
//    class_record_guid  content.db's `class` row of the record's class (ECharacterClass, by its name)
//    customization    the record's appearance_json (the client's CharacterCustomization JSON)
//                     read by name into repl::CharacterCustomization (below)
//    stats            plan B3: the stats of the character's class at its level wearing its items, computed by the stat
//                     engine (stats/stat_engine.h) from the stat program of content.db and the worn items' stat blocks
//                     (stats/gear.h).  They fill the StatsComponent's arrays by each stat's audience mask; the
//                     captured Bard's item ids, keys and order are kept for the stats it has (so the captured
//                     Bard in its kit gives pkt#107 bit for bit), the stats a class has that the Bard does not
//                     (its resource) take the next ids and the Bard's own are left out
//    abilities        the captured Bard's lists (spec section 5), whatever the class
//    spawn            the record's position (x, y, z in cm, yaw in degrees) or, when it has
//                     none (no save yet), the default spawn: the captured character's
//                     (transcript section 4.1; kDefaultSpawn)
//    items            plan B2: the record's items (svc::Character::items) as the Inventory the session plays
//                     (CharacterSetupResult::inventory), the PlayerSetup's `items` (the instances the join's storage payloads and
//                     events carry) and the visible gear derived from what is worn: HeldItems (the 23 slots, each the record worn in it)
//                     and AppearanceIDs (the worn armor, costume and cloak by slot), as items/inventory.h says.  The captured Bard's kit
//                     gives the captured character's own arrays.  An item the table or the rules refuse is a refused login: playing on
//                     without it would save a smaller inventory over the stored one
//    everything else  the PlayerSetup defaults (the captured level-1 extras), the login and
//                     the session values as passed
//
//  Race, gender and class.  The record's race_id, gender_id and class_id are the client's enums
//  (plan B1; services/internal/chardef): ECharacterRace 0 Kaelar, 1 Vaelune, 2 Vek, 3 Renkai,
//  4 Empyrean, 5 Pyrai, 6 Dunir, 7 Nikua, 8 Tulnar (the exe's name table at 0x14b74e200 and the
//  switch at 0x146a8d2c0 that turns the value into "race.kaelar" ...); ECharacterGender 0 Male,
//  1 Female; ECharacterClass 1 Ranger, 2 Bard, 3 Cleric, 4 Fighter, 5 Rogue, 6 Summoner, 7 Tank,
//  8 Mage.  They are what the character-select protocol carries (CreateCharacterInfo gender 2,
//  race 3, primary_class 4) and what the pawn's Race and Gender are, so there is no table: race 0-7
//  and gender 0-1 are the wire's race and gender, and every race joins.  Tulnar (8) has no player
//  appearance in the design data and is never created, and an id past the enum is a corrupt record:
//  both are refused as an unsupported race (login_text::kRaceUnsupported).  The class is looked up
//  in content.db by its name.  (Until plan B1 the services stored the class in race_id, and worldd
//  mapped 2, 7 and 8, the class ids of a Bard, a Tank and a Mage, as "races".)
//
//  Customization.  The client's JSON (FJsonObjectConverter of FCharacterCustomizationSaveData):
//  member names in lowerCamel (`skinColorHue`), floats as numbers (the double of a float32),
//  64-bit ids as decimal strings, `bIsHelmetVisible`/`bIsCapeVisible` as booleans,
//  `decalData`/`decalBlendGroups` as arrays of objects, `sectionsValues` as an object of the
//  body section enum's names (lowerCamel) to {x, y, z} doubles, `faceMorphWeightMaps` as an
//  object of morph names (first letter lowercased) to floats; plus `gender`, `race` and `class`
//  as names, which are cross-checked and not otherwise used.  It becomes the wire struct:
//    - each of the 30 scalar members present in the JSON becomes a delta (the client sends a
//      member only when it differs from a default the wire does not carry; sending one that
//      equals the default is the same value);
//    - Race and Gender go out as bytes (the client has no preset for presetGuid 0); a non-zero
//      presetGuid is an error, because whether the client's preset table has it, and so
//      whether those bytes are on the wire, is not known;
//    - a morph name is the key with its first letter capitalised (the capture's FNames are:
//      "Nose_Bridge_Hooked_0" for the JSON's "nose_Bridge_Hooked_0"; FName compares without
//      case) split into plain text and number the way FName does ("_0" is number 1);
//    - sectionsValues keys are looked up in the section enum (None 0 ... cCount 29).
//  customization_to_json is the inverse for the wire value (the JSON form of the capture's
//  own customization, for the proofs); members absent from the struct are absent from the JSON.
// ============================================================================
#pragma once

#include "bootstrap/player_setup.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "session/action_bar.h"
#include "session/leveling.h"
#include "session/talents.h"
#include "stats/character_stats.h"
#include "svc/services.h"
// (Cyb ~ C.O.A)

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aoc::worldd::bootstrap {

// ---- race and gender --------------------------------------------------------------------------------------------------

/// The client's race for a record's race_id (ECharacterRace): 0 Kaelar to 7 Nikua; nullopt for 8 (Tulnar, which cannot be
/// created) and for any other value.
std::optional<ClientRace>   client_race_from_id(int32_t race_id);
/// The client's gender for a record's gender_id (ECharacterGender): 0 Male, 1 Female; nullopt for any other.
std::optional<ClientGender> client_gender_from_id(int32_t gender_id);
/// The name of an ECharacterClass id ("Ranger" 1, "Bard" 2, "Cleric" 3, "Fighter" 4, "Rogue" 5, "Summoner" 6, "Tank" 7,
/// "Mage" 8), which is also the class's name in content.db; nullptr for 0 and any other value.
const char* client_class_name(int32_t class_id);

/// The names the customization JSON and the exe's enum use ("Kaelar", "Male"); nullptr for a value past the enum.
const char* client_race_name(ClientRace race);
const char* client_gender_name(ClientGender gender);
std::optional<ClientRace>   client_race_from_name(std::string_view name);
std::optional<ClientGender> client_gender_from_name(std::string_view name);

// ---- customization ----------------------------------------------------------------------------------------------------

struct CustomizationFromJson {
    std::optional<repl::CharacterCustomization> value;      // empty when error is set
    std::string                                 error;
    /// Top-level keys of the JSON that nothing is read from (a member this reader does not know). `race`, `gender`
    /// and `class` are read (race and gender are checked against the arguments) and are not listed.
    std::vector<std::string>                    ignored_keys;
};

/// The most appearance JSON that is parsed at all (cyb's is 3 KB; the wire value of anything near this cannot be
/// sent in one chain anyway).  A record beyond it is refused before the parser sees it.
inline constexpr size_t kMaxAppearanceBytes = 64 * 1024;

/// The wire value of the client's customization JSON for a character of `race` and `gender` (the record's, mapped).
/// An error for JSON over kMaxAppearanceBytes or that does not parse, a member of the wrong type or out of range, a
/// non-zero presetGuid, a race or gender name that names another than `race` / `gender`, or a value the wire cannot
/// carry (a morph name used twice ...).
CustomizationFromJson customization_from_json(std::string_view appearance_json, ClientRace race, ClientGender gender);

/// The client's JSON form of a wire value, in the client's member order: the members the struct holds (a decal or
/// blend group that was not sent is null), `race` and `gender` from its race_gender when it has one; presetGuid and
/// randomSeed are 0 when absent.  Compact text.  nullopt for a value JSON cannot hold (a NaN or infinite float, a
/// hardcoded morph name, a section byte past the enum).
std::optional<std::string> customization_to_json(const repl::CharacterCustomization& c);

/// The names of FCharacterCustomizationBodySections in enum order (None 0 ... cCount 29), for the sectionsValues keys.
inline constexpr size_t kBodySectionCount = 30;
const char* body_section_name(uint8_t value);

// ---- the default spawn ------------------------------------------------------------------------------------------------

struct Spawn {
    double x, y, z;         // cm
    double yaw_degrees;
};

/// Where a character with no saved position spawns: the captured character's spawn point (pkt#22 SpawnLocation) and
/// the yaw whose compressed short is 16550 (pkt#78 rotation).  The world's real spawn points are a later plan.
inline constexpr Spawn kDefaultSpawn = {-777664.5, 616578.6875, 15952.0498046875, 16550.0 * 360.0 / 65536.0};

// ---- the setup ----------------------------------------------------------------------------------------------------------

/// The per-connection values of the session (PlayerSetup's [sess] part).
struct SessionValues {
    uint32_t     server_id = 0;
    double       world_time = 0;
    int32_t      player_id = 0;
    SessionGuids guids;
};

/// Why a setup was refused, as far as the player's text tells it apart (the log has the sentence).
enum class SetupRefusal {
    None,
    UnsupportedRace,   // Tulnar, or a race id past the enum (login_text::kRaceUnsupported)
    Other,             // everything else (login_text::kLoginFailed)
};

/// What build_player_setup computes the stats with (plan B3): the verified stat program, where the items' stat data comes from, and how an
/// item's interpolated value is rounded. All three outlive the setup and the character's CharacterStats, which keep pointers to them.
struct StatContext {
    const stats::StatBundle*  bundle = nullptr;
    const stats::GearCatalog* gear = nullptr;
    stats::Rounding           rounding = stats::Rounding::HalfUp;
    /// Plan D1: the Adventuring level curve and the Universal tree's auto-granted nodes (the ranks a level grants, whose constant
    /// stat mods the stats take as an input). Null: the setup's progress is the level's threshold and no rank is granted.
    const leveling::LevelCurve*            curve = nullptr;
    const std::vector<content::AutoGrant>* grants = nullptr;
    /// Plan D1, stage 2: the talents catalog. With it the setup lists the XPTracker's trees from the character's ranks, derives the
    /// KnownAbilities from the class's tree and the weapon in hand (not the captured Bard's), feeds the ranks' constant mods to the stats and
    /// makes the action bar; without it (a test's context) the captured Bard's lists and the Universal tree's auto grants stay.
    const talents::Catalog*                talents = nullptr;
    /// The mounts milestone: the mount tables. With them a mount item in the bag or worn brings its ability (Summon_Mount) to the pawn's
    /// ReplicatedRecordsToLoad (carried_item_abilities); null: no item brings one.
    const content::MountData*              mounts = nullptr;
};

/// Plan D1 (effects since plan D2): the passives of the ranks `granted` (the Universal tree's auto grants a level reached) that have constant
/// stat mods, one infinite effect each, for the stat engine's effect state.
std::vector<stats::ActiveEffect> rank_effects_of(std::span<const content::AutoGrant* const> granted);
// ~ C-O-A & Cyb ~

struct CharacterSetupResult {
    std::optional<PlayerSetup> setup;
    /// Why there is no setup, as one sentence for the log; `refusal` picks the player's text.
    std::string                error;
    SetupRefusal               refusal = SetupRefusal::None;
    /// What the caller should log: the default spawn used, JSON keys not read, a rounding-ambiguous attribute, a class
    /// name that disagrees with the archetype.  Never errors.
    std::vector<std::string>   notes;
    /// Plan B2: the character's items as the session plays them (set whenever `setup` is).
    std::optional<items::Inventory> inventory;
    /// Plan B3: the character's stats as the session plays them: started with the worn items, what `setup.stats` was made from (set
    /// whenever `setup` is).
    std::optional<stats::CharacterStats> character_stats;
    /// Plan D1: the character's Adventuring progress (the record's level; its XP row when it has one at or past the level's threshold,
    /// else the threshold; the debt).
    leveling::Progress                   progress;
    /// Plan D1, stage 2: the character's ranks (the catalog's rules over what the record stored) and its action bar (the stored one, or the
    /// default: the weapon bank and the class's auto-granted skills). Set when the context has the catalog.
    std::optional<talents::Book>         book;
    actionbar::Bar                       action_bar;
    /// The weapon line of what is worn in the Primary slot (0: none): a weapon tree's passive ranks apply while its line is in hand.
    int64_t                              wielded_line = 0;
};

/// Plan D1, stage 2: what a worn weapon gives: its combo ability (WeaponCombo_*) and its weapon line.
struct WeaponInfo {
    int64_t     combo = 0;
    int64_t     line = 0;
    std::string line_name;
};

/// The weapon worn in an equip slot (Primary 15, Ranged 17): nullopt when nothing is worn there or the item is no weapon (it names no weapon
/// line). `error` is set when content.db could not be read.
std::optional<WeaponInfo> worn_weapon(const content::ContentDb& content, const items::Inventory& inventory, uint8_t slot, std::string* error = nullptr);

/// The combos of the weapons the character carries, worn ones by slot then the bag's by slot, each once.
std::vector<int64_t> carried_combos(const content::ContentDb& content, const items::Inventory& inventory);

/// The abilities the items the character carries bring with them, each once, in the order the items lie (the bag, then what is worn).  The client's
/// AbilityComponent loads only what ReplicatedRecordsToLoad lists, and an item use that runs through an ability (a mount item: ActivateItem's SummonMount
/// and CastAbility branches call TriggerAbility) fails with "Ability not loaded" and sends no ServerTriggerAbility unless the ability is loaded; retail lists
/// the ability of every item in the bag and on the body (LoadBackpackAbilities, LoadEquipmentAbilities).  Modelled: a mount item's Summon_Mount.  Empty with
/// no mount tables.
std::vector<int64_t> carried_item_abilities(const content::MountData* mounts, const items::Inventory& inventory);

/// The AbilityComponent's General_Unstuck (the first KnownAbilities element of every join) and the static tail of 45 abilities that ends
/// it (the fishing abilities, Rest, the emotes, Dodge, Guard, Sprint ...): constant in every capture, taken from the captured Bard's list.
int64_t              general_unstuck();
std::span<const int64_t> static_known_tail();

/// The ReplicationID the captured Bard's HeldItems array had when it was built (pkt#107's ArrayReplicationKey): 23 items added, then 23 marked.
inline constexpr int32_t kInitialHeldItemsKey = 46;

/// The 23 entries of HeldItems for what `inventory` has worn: ReplicationIDs 1..23 in the order of items::kHeldSlots, each entry's item the
/// record worn in that slot (0: none), enchantment 0.
std::vector<HeldItem> held_items_for(const items::Inventory& inventory);

/// The items of the Equipment storage as the stat engine takes them: record, quality and EEquipSlot, by ascending slot.
std::vector<stats::WornItem> worn_items_of(const items::Inventory& inventory);

/// The StatsComponent's arrays for the character's stats (plan B3): each stat goes by its audience mask to the Owner (1), OwnerProxy (5) or
/// EveryoneProxy (7) array; a mask of 6 (other players' views) or 0 is not sent to the owner, and a Type_FloatArray stat (TempHealth) goes
/// to the float array. `layout` has the arrays as the original server sent them (the captured Bard's): its items keep their ids, keys and
/// order for the stats that are still there, and the stats it lacks (another class's resource) are added with the next ids after its highest
/// (its keys stay as they are: the capture's are below its highest ids, the server's own history). A stat that replicates base and
/// equipment carries both. nullopt, with `error`, when the
/// values name a stat twice.
std::optional<StatsSetup> stats_setup_for(const std::vector<stats::StatValue>& values, const StatsSetup& layout, std::string& error);

/// Builds the PlayerSetup of `character` (see the top of this file).  `login` and `session` are copied in.
CharacterSetupResult build_player_setup(const svc::Character& character, const content::ContentDb& content,
                                        const LoginIdentity& login, const SessionValues& session, const StatContext& stats);

/// The character id's 32 hex digits (either case) as four big-endian words; nullopt for anything else.
std::optional<std::array<uint32_t, 4>> character_guid_words(std::string_view hex32);

}  // namespace aoc::worldd::bootstrap

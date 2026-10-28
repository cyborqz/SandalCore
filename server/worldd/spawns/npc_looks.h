// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/npc_looks.h  (people's looks and names, step 0)
//
//  How a humanoid NPC (a guard, a villager, a shopkeeper, a clerk, a quest giver) looks and what it is called.
//
//  The body.  A humanoid's mesh is the player's master mesh and its body is merged by the client from the NPC's UNPCAppearanceComponent, a dynamic subobject of its
//  open.  The component is replicated: the client builds the body from what the open carries and derives nothing from the NPCAppearance record, so the server makes the
//  choices, as the game's server does when it makes the NPC:
//    * the NPCAppearance record: the population asset's AppearanceOverrideId when that is a record, else the one the Blueprint's class defaults name (npc_looks.tsv);
//    * its shared appearance info (SharedAppearanceInfoId) goes into the component as it is;
//    * one OUTFIT of the record's options, drawn with the record's weight curve;
//    * one item of each slot list of the outfit (head, chest, wrists, legs, shoulders, back, waist, hands, feet, held items), drawn with the outfit's weight curve,
//      in the order the game adds them: that is AppearanceIDs;
//    * the customization: Race and Gender (the Blueprint's, or one of the pairs the shared info allows when the record says to randomize them), a fresh RandomSeed and
//      the colours, hair and eyes.  Modelled: the game fills the colours with its character creator's randomizer, which is not ported; the faces are taken from the
//      customizations the retail captures hold for the same shared info, race and gender (a pool), and a race the captures hold none of gets the plain defaults.
//  The weight curves are the data's: the weights are the curve sampled at the item's place in the list (the list spread over the curve's keys), moved up to be
//  non-negative, and the draw is over their running sum; a curve with no keys weighs every item the same.
//
//  The name.  The first non-empty list of display names of the population instance, the asset set's entry for the asset and the asset; one entry of it is drawn.  With
//  none, the Blueprint's own name (its class defaults' CharacterName, npc_looks.tsv), which the game's server copies into the NPC's info; with that empty too (a villager) the
//  open carries no CharacterName.  Never the name an open was recorded with.
//
//  The pose.  Every NPC the data makes (a person or a creature) gets the idle animation its population instance names, as the game's server sets it when it makes the NPC: the
//  client plays it while the NPC stands (sitting, kneeling, lying wounded, arms crossed, reading ...).  An instance that names none leaves the field out; a recorded open's own
//  pose is the recording's and goes.  Quest givers, whose state tree only idles, stand where they are put (npc/area_npcs.h, load_standing_archetypes).
//
//  Who is dressed.  A humanoid Blueprint (master mesh) that is not hostile to players (its starting alignments, against the player's, by the combat tables) and whose look
//  resolves to a record with a shared info and an outfit.  The hostile ones (bandits, cultists) stay out of the world until their fights are read.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "npc/motion_table.h"
#include "protocol/repl/value.h"
#include "spawns/captured_spawns.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace aoc::worldd::spawns {

struct DataSpawn;

/// One of the data's weight curves: keys of time and value, each with the mode that leaves it (L linear, C cubic with the data's zero tangents, K constant); constant
/// outside the keys.
class LookCurve {
public:
    static LookCurve parse(const std::string& text);
    bool   empty() const { return keys_.empty(); }
    size_t keys() const { return keys_.size(); }
    double eval(double time) const;
    double first_time() const { return keys_.empty() ? 0 : keys_.front().time; }
    double last_time() const { return keys_.empty() ? 0 : keys_.back().time; }
    double min_value() const;

private:
    struct Key {
        double time = 0, value = 0;
        char   mode = 'K';
    };
    std::vector<Key> keys_;
};

/// The index a draw `u` (0 up to 1) picks among `count` items weighted by `curve`: -1 for none, 0 for one item (no draw), else as the game's server draws.
int weighted_index(size_t count, const LookCurve& curve, double u);

/// The number the client gives an idle animation by the name the population data uses ("None" is 0, "Wounded_Lying" 16); -1 when the name is not one the client knows.
int idle_animation_number(const std::string& name);

/// Sets the idle animation the NPC's population instance names in the NPC's secondary info, as the game's server does when it makes the NPC: the instance's number, and no value at
/// all for "None" (so a recorded open's own pose goes).  Nothing changes for an NPC with no instance (a recorded one keeps its pose) or no secondary info.  False, with `error`, when
/// the name is unknown or the value is refused; the NPC then keeps the open's own.
bool apply_idle_animation(world::WorldActor& w, const content::NpcInstanceRow* instance, std::string& error);

class NpcLooks {
public:
    /// Reads npc_looks.tsv, npc_appearances.tsv and npc_names.tsv of `dir` (dist/content).  `motion` says which Blueprints are humanoid and what they start as; `combat` (may be null:
    /// nobody is hostile) which alignments are the players' enemies; `captured` holds the recorded customizations of the pool.  nullptr, with `error`, when a file is missing or malformed.
    static std::unique_ptr<NpcLooks> load(const std::string& dir, const npc::MotionTable* motion, const content::CombatRows* combat, const CapturedData* captured, std::string& error);

    /// The NPCAppearance record the NPC of `asset` is made from: its override when that is a record, else its Blueprint's default; 0 for none.
    int64_t appearance_of(const content::NpcAssetRow& asset) const;
    /// Whether the asset's Blueprint is a humanoid (its body is merged), hostile to players or not.
    bool is_humanoid(const content::NpcAssetRow& asset) const;
    /// Whether a person of `asset` can be dressed and named: a humanoid that is not hostile to players, with a look that resolves.
    bool can_dress(const content::NpcAssetRow& asset) const;
    /// Whether the open of a person of this instance and asset needs the interactable subobject (it has something to say or sell).
    static bool talks(const content::NpcAssetRow& asset, const content::NpcInstanceRow* instance);

    /// The name the data gives the NPC of this instance and asset (drawn by `seed`): nullopt when no list names it (the Blueprint's own name stands).
    std::optional<std::string> name_of(const content::NpcInstanceRow* instance, const content::NpcAssetRow& asset, int64_t asset_set, uint64_t seed) const;

    /// Dresses and names the NPC `w` made for `s` from a humanoid open.  False, with `error`, when the NPC has no look to change (the template carries no appearance component) or a value is
    /// refused; the NPC then stays as the open made it.
    bool dress(world::WorldActor& w, const DataSpawn& s, std::string& error) const;

    /// What the files hold, for the log.
    std::string describe() const;
    size_t blueprints() const { return blueprint_.size(); }
    size_t appearances() const { return appearance_.size(); }
    size_t outfits() const { return outfit_.size(); }
    size_t pool_kinds() const { return pool_.size(); }
    /// The counts of dress() since the start.
    struct Counts {
        mutable size_t dressed = 0, no_component = 0, no_record = 0, named = 0, blueprint_named = 0, unnamed = 0, from_pool = 0, plain_customization = 0;
    };
    const Counts& counts() const { return counts_; }

    // ---- the pieces, exposed for the log, the check scripts and the TEST_STEPS' probes ----
    struct BlueprintLook {
        int64_t     appearance = 0;
        std::string name;        // the name its class defaults give ("" none)
        std::string info_name;   // ... the one its info component carries itself: the open leaves the name out when it equals this
        int         race = 0, gender = 0;
    };
    struct Appearance {
        std::string          name;
        int64_t              shared_info = 0;
        bool                 randomize_custom = false, randomize_race_gender = false, hair_natural = true;
        int64_t              seed = 0;
        LookCurve            curve;
        std::vector<int64_t> outfits;
    };
    struct Outfit {
        std::string                          name;
        bool                                 single_curve = true;
        LookCurve                            curve;
        std::map<int, std::vector<int64_t>>  items;   // slot -> the item appearance records of its list
        std::map<int, LookCurve>             slot_curves;
    };
    const Appearance*    appearance(int64_t guid) const;
    const Outfit*        outfit(int64_t guid) const;
    const BlueprintLook* blueprint(const std::string& archetype_path) const;
    /// The AppearanceIDs entries (record, slot) the NPC drawn by `seed` of `guid` wears; empty for a record that dresses nobody.
    std::vector<repl::AppearanceId> items_of(int64_t appearance_guid, uint64_t seed) const;

private:
    NpcLooks() = default;
    bool hostile(const std::vector<int64_t>& alignments) const;
    /// The name of the NPC (see dress()): the data's, or none at all.
    bool apply_name(world::WorldActor& w, const DataSpawn& s, const BlueprintLook* bp, uint64_t seed, std::string& error) const;

    const npc::MotionTable*                              motion_ = nullptr;
    std::map<std::string, BlueprintLook>                 blueprint_;
    std::map<int64_t, Appearance>                        appearance_;
    std::map<int64_t, Outfit>                            outfit_;
    std::map<int64_t, std::vector<std::pair<int, int>>>  races_;        // shared info -> the (race, gender) pairs it may make
    std::map<int64_t, std::vector<std::string>>          instance_names_, asset_names_;
    std::map<std::pair<int64_t, int64_t>, std::vector<std::string>> set_names_;
    std::set<int64_t>                                    hostile_alignments_;
    std::map<std::tuple<int64_t, int, int>, std::vector<repl::CharacterCustomization>> pool_;   // (shared info, race, gender) -> recorded customizations
    mutable Counts                                       counts_;
};

}  // namespace aoc::worldd::spawns

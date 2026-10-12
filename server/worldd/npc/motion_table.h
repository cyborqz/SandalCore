// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  npc/motion_table.h  (plan E1, stage 2)
//
//  What each NPC archetype's Blueprint says about the creature it makes, as tools/mapc read it from the game's containers
//  (dist/content/npc_motion.tsv, one line per Blueprint whose class descends from NPCPawn_C: 2,169 of them):
//
//    stands and moves   the capsule's half height and radius, CharacterMovement's speeds and rotation rate, the default movement mode
//    is made of         the Blueprint's parent classes, its DefaultScale, its StartingAlignments (the records a creature opens with) and its tags
//
//    draws              (plan E1, stage 3, columns 14 to 17) the SkeletalMesh and AnimClass its CharacterMesh0 resolves along the archetype chain, whether a
//                       material it overrides is the engine's placeholder and whether it is hidden: the spawn filter's "no valid visuals"
//    fights             (columns 18 to 21) the stat initializer lists its StatsComponent names, the StatDistribution weights, the elite ranks and the reward tables
//
//  A Blueprint with no line is not a pawn (a prop, a banner, a tent): worldd cannot open it as an NPC.
//
//  An NPC's origin is its capsule's centre: it stands one half height (times the actor's scale) above its ground.  In the retail captures a wolf and
//  a bear stand 96 cm above the ground (Blueprint 88, scale 1.09), a Rabid 62 to 66 (Blueprint 66), a Bonebinder 144 (Blueprint 88, scale 1.64), a
//  player 98.
//
//  A table that is missing or that has no line for an archetype gives the Blueprint defaults of NPCPawn (half height 88, radius 34, walk speed 600),
//  and says so in the log once: the NPCs then stand at the ground plus 88.
// ============================================================================
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace aoc::worldd::npc {

struct MotionStats {
    double half_height = 88.0;     // cm
    double radius = 34.0;
    double max_walk_speed = 600.0; // cm/s
    double max_run_speed = 600.0;
    double max_acceleration = 2048.0;
    double rotation_rate_yaw = 360.0;   // degrees per second
    // what the Blueprint is made of (columns 7 to 13 of the file)
    std::string          parents;            // the parent classes, nearest first, comma separated ("Goblin_C,NPCPawn_C")
    double               default_scale = 1.0;
    std::vector<int64_t> alignments;         // StartingAlignments: CharacterAlignment record guids
    std::string          move_mode;          // DefaultCharacterMovementMode ("EMovementMode::MOVE_NavWalking")
    double               fly_speed = 0;      // CharacterMovement's MaxFlySpeed (0: the Blueprint sets none)
    std::string          tags;               // GameplayTags, comma separated
    // what the Blueprint draws (columns 14-17; a file with 13 columns has none of this: every Blueprint is then taken to draw)
    bool                 has_visuals_columns = false;
    std::string          mesh;               // the SkeletalMesh of CharacterMesh0 ("SkeletalMesh'SK_NPC_Wolf_A_0'"; "" none)
    std::string          anim;               // its AnimClass ("" none: the mesh shows in a T-pose)
    bool                 placeholder = false;   // one of its overridden materials is the engine's pink placeholder
    bool                 hidden = false;        // the actor or its mesh is hidden
    // what it fights with (columns 18-21; zero when the file has 13 or 17 columns)
    bool                 has_combat_columns = false;
    int64_t              base_list = 0, list1 = 0, list2 = 0;   // BaseStatInitializer, StatInitializerList1 (NPC_Base), StatInitializerList2 (the elite scalar)
    int                  dist[8] = {10, 10, 10, 10, 10, 10, 10, 10};   // StatDistribution: Constitution, Strength, Dexterity, Mentality, Intelligence, Wisdom, MaxHealth, MaxMana
    std::vector<std::pair<int, int>> elite;   // EliteLevelRanks: (level, rank)
    std::vector<int64_t> rewards;            // the RewardTables of the Blueprint's Rewards
    /// The mesh is the player's master mesh: the body is merged at run time from an NPCAppearanceComponent, which a generic open does not carry.
    bool appearance_dependent() const { return mesh.find("SK_PC_MASTER") != std::string::npos; }
};
// (C | O | A + Cyb)

class MotionTable {
public:
    /// Loads the file; false with `error` when it is missing or malformed (the table stays empty).
    bool load(const std::string& path, std::string& error);
    /// Parses the file's text (the tests).
    bool parse(const std::string& text, std::string& error);

    /// The line of `archetype_path` ("/Game/.../Wolf.Default__Wolf_C"); none when the table has no such line.
    std::optional<MotionStats> find(const std::string& archetype_path) const;
    /// The line, or the defaults.
    MotionStats get(const std::string& archetype_path) const;
    /// Every line, by archetype path.
    const std::map<std::string, MotionStats>& rows() const { return rows_; }
    size_t size() const { return rows_.size(); }
    bool empty() const { return rows_.empty(); }

private:
    std::map<std::string, MotionStats> rows_;
};

}  // namespace aoc::worldd::npc

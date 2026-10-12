// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/mount_spawn.h  (the mounts milestone, stage 2)
//
//  A summoned mount as a world actor (world/actor_registry.h, ActorKind::Mount).  Retail opens one ~104 ms after the summon request, as an
//  ANPCCharacter-layout actor with eight subobjects (research/mounts/evidence.md 2, captures 911, 898, 871; tools/capture/gen_mount.py):
//
//    actor            ANPCCharacter: ReplicatedMovement (at rest), RemoteRole 1, Role 4, Instigator = the summoner's pawn, the server's timestamp,
//                     movement mode 1.  No Owner and no Controller (an NPC has its AI controller there; a mount's are set when someone rides it: stage 3).
//    subobjects       AlignmentComponent, InteractInfo (UNPCSecondaryInfo: the owner's guid), BaseCharacterInfo (UNPCInfo: the mount's own CharacterGuid,
//                     the owner's name), CombatInfo, AbilityComponent (MountAbility_Sprint to load), StatsComponent, <Kind>MountComponent (UMountComponent:
//                     the item's guid, bIsSummoning, DissolveTimer), InteractableComponent (Mount_Interaction, keyed by the owner's guid)
//
//  NetGUIDs.  The actor's id is A; A+2 its CollisionCylinder (named by the hits and, when someone rides it, by the player controller's attachment), then the
//  subobjects at A+4 ... A+14 in the order above, A+16 which is never exported, the MountComponent at A+18 and the InteractableComponent at A+20: 93 of 93
//  captured exports (the NPC layout of spawns/captured_spawns.h without the AI controller slot, and the mount component in the second spare slot).
//
//  The archetype.  A donor is the retail open of the same kind of mount (ground 911, aquatic 898, flying 871); the item's Blueprint decides which export the
//  open carries: the donor's own when the item's Blueprint is the donor's, else the Blueprint's class default object by its path with checksum 0 and a
//  NetGUID worldd mints (the E1 generic-open path, spawns/generic_npc.h: the client does not compare a zero checksum).  The three captured Blueprints keep
//  their retail export; the others, the GM Eagle Gryphon (Mount_Flying_Gryphon_5_Armored) among them, are made.
//
//  What is the instance's own: where it stands and its yaw; the owner (its CharacterGuid in the alignments, the creature-owner guid and the interaction
//  state; its name; its pawn as the Instigator); the mount's own CharacterGuid; the item's guid; Health, MaxHealth (Mount_Base_HP at the owner's level times the
//  item's factor: x5 for a flyer) and the StealthLevel (the owner's level); no road effect (roads are stage 3's); bIsSummoning 1 and a DissolveTimer.
//  Also the instance's own (stage 3 fix 2): the alignments (the mount INHERITS its owner's: a donor's belong to the player who was captured, 898's was a Combatant, the
//  PvP flag) and the rider's AnimBP (the interaction data names it and the open exports the class: without it the rider stands on the mount).
//  What is the capture's, kept: everything else (CharacterType 1.1, the penetrations, the interaction definition, the ability list).
//  Modelled, not retail: the open has no ReplicatedBasedMovement (retail names the ground object the mount stands on, which worldd has no export for: the NPCs of E1 go without it too).
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/replicator.h"
#include "spawns/captured_spawns.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace aoc::worldd::spawns {
// C Y B + C | O | A

/// The stats of a mount's StatsComponent the maker sets (content.db's stat_def guids).
inline constexpr int64_t kStatMoveSpeedMult = 109228544644729ll;
inline constexpr int64_t kStatStealthLevel = 6064633186496741774ll;

/// An aquatic mount's dash is MountAbility_Sprint_AllowSwimming, on slot 1 of bank 18 [R: capture 898; the others have MountAbility_Sprint on slot 0].
inline constexpr int64_t kDashAllowSwimming = 6064633300902020596ll;

/// What a mount puts on bank 18 of the player's action bar: its dash and the slot.
struct DashBar {
    int64_t ability = 0;
    int     slot = 0;
};

/// A summoned mount's own values.
struct MountParams {
    uint64_t                             player_id = 0;      // the owner's PlayerId: only its connection is sent the mount
    repl::GuidValue                      owner_guid;         // the owner's CharacterGuid
    std::string                          owner_name;
    protocol::emit::FIntrepidNetworkGUID owner_pawn;         // the Instigator
    int                                  owner_level = 1;    // Mount_Base_HP is read at this level
    uint32_t                             server_id = 0;      // UNPCInfo.ServerId
    double                               world_time = 0;     // ReplicatedServerLastTransformUpdateTimeStamp
    repl::GuidValue                      item_guid;          // the used item's instance guid (UMountComponent.ItemInstanceGuid)
    double                               x = 0, y = 0, z = 0;   // cm
    double                               yaw_degrees = 0;
    uint64_t                             seed = 0;           // the mount's CharacterGuid words and its DissolveTimer: 0 takes the actor's id
    /// Set by the tests that rebuild a retail open from its own values: the mount's CharacterGuid and the DissolveTimer as the capture had them.
    std::optional<repl::GuidValue>       mount_guid;
    std::optional<float>                 dissolve_timer;
    /// The health the mount starts at (a mount that was hurt and came back: stage 3's saved Health); unset: its maximum.
    std::optional<double>                health;
    /// The owner's own alignments (its pawn's UAlignmentComponent.Alignments, as they are now): the mount INHERITS them, as retail's does (the mount's
    /// InheritedAlignments are its owner's, captures 911, 898, 871, 872, 917).  Unset: a player with no PvP flag, node or guild (Player, NonCombatant, the
    /// owner's Instigator and PVPSuppressed_Perceiver: the alignments of worldd's own pawn).  Fix 2 of stage 3: the donors' alignments belong to the CAPTURING
    /// players (898's owner stood in a Lawless zone and was a Combatant: the PvP flag; the others had a node and a guild) and must never reach a mount of ours.
    std::optional<repl::AlignmentContainer> owner_alignments;
};

/// What the maker made besides the actor, for the caller's bookkeeping.
struct MountMade {
    world::WorldActor actor;
    repl::GuidValue   mount_guid;
    double            max_health = 0;
};

class MountBook {
public:
    /// Reads content.db's mount tables and the captured donors. nullopt, with `error`, when they do not read.
    static std::optional<MountBook> load(const content::ContentDb& db, std::string& error);
    /// A book from parts (the tests').
    MountBook(content::MountData data, MountDonors donors) : data_(std::move(data)), donors_(std::move(donors)) {}

    const content::MountData& data() const { return data_; }
    const MountDonors&        donors() const { return donors_; }
    /// The mount an item summons; nullptr for an item that is none.
    const content::MountRow* find(int64_t item) const { return data_.find(item); }
    /// The Summon_Mount ability's guid (0: none in the data).
    int64_t summon_ability() const;
    /// The dash (MountAbility_Sprint) ability's guid (0: none).
    int64_t dash_ability() const;
    /// The dash of a mount's bank-18 bar: MountAbility_Sprint on slot 0, an aquatic mount's MountAbility_Sprint_AllowSwimming on slot 1.
    DashBar dash_bar(const content::MountRow& row) const;

    /// The donor open of a mount's kind; nullptr when the captures have none of it.
    const OpenTemplate* donor_of(const content::MountRow& row) const { return donors_.donor(row.kind); }

    /// The export of the item's Blueprint class default object: the donor's own when it is the donor's, else made from the path (checksum 0). Stable: the same
    /// path always gives the same NetGUIDs, so what the client acknowledged is not exported again. Empty when the row names no Blueprint.
    repl::ExportChain archetype_of(const content::MountRow& row, const OpenTemplate& donor) const;

    /// The Blueprints' speeds (dist/content/mount_speeds.tsv, tools/mapc `mount-speeds`): walk, run, fly and swim in cm/s by the Blueprint's name. False, with `error`,
    /// when the file does not read (a missing file leaves the table empty: a mount then runs at 600 cm/s times its item's ground percentage).
    bool load_speeds(const std::string& path, std::string& error);
    struct Speeds {
        double walk = 0, run = 0, fly = 0, swim = 0;
    };
    const Speeds* speeds_of(const std::string& blueprint) const;
    size_t speeds_known() const { return speeds_.size(); }
    /// Sets one Blueprint's speeds (the tests').
    void set_speeds(const std::string& blueprint, Speeds s) { speeds_[blueprint] = s; }
// CoA : C.Y.B

    /// The rider's AnimBP of each Blueprint (dist/content/mount_riders.tsv, tools/mapc `mount-speeds` writes it with the speeds): the class path
    /// "/Game/Characters/Animations/Mounts_Rider/Swiftclaw/ABP_PC_MNT_Swiftclaw.ABP_PC_MNT_Swiftclaw_C" by the Blueprint's name.  The mount's
    /// InteractableComponent names it (InteractionInputData.AttachmentAnimation) and the client puts the rider's mesh on it when the rider mounts: without it
    /// the rider keeps its on-foot AnimBP and STANDS on the mount (the client logs "Unexpected invalid anim instance when initializing rider anim proxy").
    /// False, with `error`, when the file does not read; a Blueprint it does not name (or no file) gets its kind's captured AnimBP (below).
    bool load_riders(const std::string& path, std::string& error);
    size_t riders_known() const { return riders_.size(); }
    /// Sets one Blueprint's rider AnimBP (the tests').
    void set_rider_anim(const std::string& blueprint, std::string class_path) { riders_[blueprint] = std::move(class_path); }
    /// The class path of the AnimBP the rider of `row` is put on: the file's for its Blueprint, else the one the retail donor of its kind named (ground:
    /// Swiftclaw, the terror bird's, capture 911; aquatic: Tidesnapper, the Shoalskipper's, 898; flying: Dawnbreaker, 871).
    std::string rider_anim_of(const content::MountRow& row) const;
    /// The export of that AnimBP class (the class, its package: flags 5, checksum 0, NetGUIDs worldd mints from the path, stable between calls): what the actor's
    /// open carries so that the client can find the class its AttachmentAnimation names.  Empty for a path without a dot.
    repl::ExportChain rider_anim_export(const std::string& class_path) const;

    /// MaxHealth of the mount at its owner's level: Mount_Base_HP times the item's factor.
    double max_health(const content::MountRow& row, int owner_level) const;

    /// Makes the mount actor of `row` (the layout and the values above). `mint` gives the NetGUIDs (the actor's, then every second one, as the allocator does).
    /// nullopt, with `error`, when the captures have no donor of its kind, or a value is refused.
    std::optional<MountMade> make(const content::MountRow& row, const MountParams& p, const Minter& mint, std::string& error) const;

private:
    content::MountData                       data_;
    MountDonors                              donors_;
    mutable std::map<std::string, repl::ExportChain> made_;     // by the CDO's path
    mutable std::set<uint64_t>               minted_;
    std::map<std::string, Speeds>            speeds_;
    std::map<std::string, std::string>       riders_;     // Blueprint name -> the rider AnimBP's class path
    mutable std::map<std::string, repl::ExportChain> rider_exports_;   // by the class path
};

/// The values of an ObjectState (and its fast arrays' items) with the four words of `from` replaced by `to`, wherever they stand as an FGuid, as the four
/// consecutive ints of an array element (the interaction state's StateGuids) or in an alignment or a guid map. Returns how many places changed.
size_t replace_guid(repl::ObjectState& o, const repl::GuidValue& from, const repl::GuidValue& to);

}  // namespace aoc::worldd::spawns

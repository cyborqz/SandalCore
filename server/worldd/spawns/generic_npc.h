// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/generic_npc.h  (plan E1, stage 2 fix)
//
//  The NPCs no retail capture opened.  A client opens an NPC from its archetype's export (the Blueprint's class default object by path, in its
//  package) and the actor's own blocks; stage 1 took the exports from captured opens, so a PopulationAsset whose Blueprint no capture opened (385
//  of the 437 the area's points name) had no NPC.  The design data names every Blueprint's path (PopulationAsset.AssetRef), and the exe says why
//  nothing else is needed:
//
//    The client does not compare a zero checksum (read from the exe's own code, AOCClient-Win64-Shipping.exe, not from a log line):
//      * `UPackageMapClient::InternalLoadObject` (function at 0x1442740f0) reads the export's NetworkChecksum when the export flags have bit 2
//        (the "%s has network checksum %u" log, 0x1442745e3) into a local; at 0x1442748b8 `cmp dword ptr [rbp-0x31], 0 / je 0x144274a40` jumps over the
//        whole check when it is 0 ("Default object package network checksum mismatch!" is logged only past that jump, at 0x1442749b2, and only when the
//        loaded object's checksum differs);
//      * the guid cache's own check (the code at 0x144270347, which logs "GetObjectFromNetGUID: Network checksum mismatch" at 0x144270482 and closes the
//        connection with ENetCloseResult::NetChecksumMismatch) starts with `cmp dword ptr [rdi+0x20], 0 / je 0x144270567`: [rdi+0x20] is the cache
//        object's stored NetworkChecksum, the one the export gave it, and 0 skips the compare;
//      * both are also skipped by the CVar net.IgnoreNetworkChecksumMismatch.
//    So an export written with checksum 0 is accepted whatever the Blueprint, and no class layout needs to be computed.  (Retail's own checksum of every
//    Blueprint default object in the captures is the one constant 47039926, of its native class, and its packages' is 0.)
//
//  Such an NPC is made from a retail open of the same native class, the plain ANPCCharacter: the six standing subobjects of a creature (Alignment,
//  InteractInfo, BaseCharacterInfo, CombatInfo, Ability, Stats) with the names and checksums retail's open gave them (donor: the Wolf's).  What is
//  the Blueprint's own comes from the Blueprint (tools/mapc, npc_motion.tsv):
//
//    archetype     the Blueprint's path from the data, checksum 0, NetGUIDs of worldd's own (a hash of the path with the static bit set: retail's are
//                  such, and none of the captured ones is taken)
//    alignment     the Blueprint's StartingAlignments, then the creature's own (the instigator record with its CharacterGuid), as retail's open lists them
//    scale         the Blueprint's DefaultScale (an NPC carries it in SerializeNewActor)
//
//  and the rest from the data, by `instantiate`: PopAssetId and PopInstanceId (the client reads the nameplate, the faction and the rest of the
//  population records from them), the level range.  Modelled, not retail: the donor's stats, abilities and associated node (the stat engine's NPC
//  lists are stage 3's), and no appearance component.  The look of most creatures and humanoids is the Blueprint's own (the Goblin's and the guards' CharacterMesh0 is set in the Blueprint; their
//  retail opens carry no appearance component either); only the 165 assets with a PopulationAsset NPCAppearance have one in retail, a server-made
//  customisation that no record gives (RandomSeed, the 8 appearance ids): of the area's points 92 name only such assets, 79 of them Guard_Soldier (made from
//  the captured template when a capture opened it, whose component is kept) and 13 quest NPCs (Quest_NPC_Master and one named NPC), which show the
//  Blueprint's default.  Unproven until a run.
//
//  Huntable creatures (Gathering stage 3, "Hunting"): an asset whose PlayerInteractionDef is one of the Hunting_<kind> defs (GenericConfig::hunting_defs) gets an eighth block, a
//  UInteractableComponent, as the living Hunting_Raven and Hunting_Spider of the captures have (capture 914 packet 1561, 758 packet 4538, and a Tidesnapper's in 1458): the Raven's own block
//  with the asset's two ids put in (ComponentDefId = the asset's InteractComponent, InteractionInputData[0].PlayerInteractDefinition = its InteractionDef).  The three captured blocks differ
//  in those two ids alone, which tools/capture/gen_hunting.py and test_worldd_hunting prove to the bit.  Every other creature keeps its seven blocks.
//
//  A huntable's other blocks are a living huntable's too, not a hostile creature's (the same Pop_Raven instance of capture 914 sends both): its CombatInfo writes bIsDead, bInvulnerable and
//  bNotTargetable (all 0), its AbilityComponent is empty (it fights nothing), its UNPCInfo carries the CharacterName the nameplate shows (a hostile creature's open has none: the client reads
//  the Blueprint's), and its one Blueprint alignment is the Friendly one (npc_motion.tsv: 6064632310208593979, Friendly to the Player alignment in alignment_relation), where the hostile
//  Raven lists two hostile ones.  The first two are copied from the captured living Raven, the name is put in, and the alignment comes from the Blueprint as for every creature.
//
//  A Blueprint that is not a pawn (npc_motion.tsv has no line for it: a banner, a tent, a quest sparkle), an excluded one, and every Blueprint when
//  the table is not loaded or the switch is off, get no template, and the loader counts them by name.
// ============================================================================
#pragma once
// ~ C.Y.B & CoA ~

#include "content/content_db.h"
#include "npc/motion_table.h"
#include "spawns/captured_spawns.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::spawns {

struct GenericConfig {
    bool                     enabled = true;
    /// Blueprint names to leave out, one by one: a kill switch for a type that misbehaves on a client.  A name is the class name (`Goblin_Gravehide`,
    /// `Goblin_Gravehide_C`) or the archetype's whole path, compared without regard to case.
    std::vector<std::string> exclude;
    /// The NetworkChecksum of the exports the client is sent.  0: not compared (see above).
    uint32_t                 export_checksum = 0;
    /// Gathering stage 3, "Hunting": the PlayerInteractionDefs of the huntable creatures (gather/gathering.h hunting_defs).  Empty: every creature has its seven plain blocks.
    std::set<int64_t>        hunting_defs;
    /// ... and the name of each, which the creature's nameplate shows (UNPCInfo.CharacterName: gather/gathering.h hunting_names).  A def with no name here keeps the donor's UNPCInfo as it is.
    std::map<int64_t, std::string> hunting_names;
    /// false (worldd --no-gathering): the world does not look the defs up and no creature is huntable.
    bool                     hunting = true;
};

enum class Verdict : uint8_t {
    Generic,      // a template was made
    NotPawn,      // the Blueprint is no NPCPawn (a prop)
    Excluded,     // by name
    Disabled,     // the switch is off
    NoTable,      // npc_motion.tsv is not loaded: nothing is known of the Blueprints
    NoDonor,      // no captured open to make it from
    NoPath,       // the asset names no Blueprint
};

const char* verdict_name(Verdict v);

/// The leaf of an archetype path or class name: "Wolf" of ".../Wolf.Default__Wolf_C", of "Wolf_C" and of "Wolf" alike.
std::string blueprint_leaf(const std::string& archetype_or_class);

class GenericNpcs {
public:
    /// `blueprints` (may be null) must outlive this.
    GenericNpcs(const CapturedData& captured, const npc::MotionTable* blueprints, GenericConfig config);

    /// The open of an NPC of `asset`'s Blueprint (a captured template is the caller's first choice); nullptr with `why` when there is none.  A huntable creature's has the eighth
    /// block (above).  The pointer is valid as long as this object.
    const OpenTemplate* template_for(const content::NpcAssetRow& asset, Verdict* why = nullptr) const;

    /// What the donor is (its archetype path), "" when the captures have none.
    const std::string& donor() const { return donor_path_; }
    const GenericConfig& config() const { return config_; }
    size_t made() const { return made_.size() - hunting_made_; }   // the Blueprints' plain templates
    /// How many of the templates made are a huntable creature's (eight blocks).
    size_t hunting_made() const { return hunting_made_; }
    /// Whether the captures hold the living huntable the eighth block is made from (the Hunting_Raven's open).
    bool has_hunting_block() const { return hunt_block_ != nullptr; }

private:
    /// The plain template of the asset's Blueprint (seven blocks), made and kept; nullptr with `why` when the Blueprint has none.
    const OpenTemplate* make_plain(const content::NpcAssetRow& asset, Verdict* why) const;

    const CapturedData*      captured_;
    const npc::MotionTable*  blueprints_;
    GenericConfig            config_;
    std::set<std::string>    excluded_;      // lower case
    const OpenTemplate*      donor_ = nullptr;
    std::string              donor_path_;
    const ParsedBlock*       hunt_block_ = nullptr;   // the living Hunting_Raven's UInteractableComponent
    const OpenTemplate*      living_ = nullptr;       // ... and its whole open (its CombatInfo and AbilityComponent are every huntable's)
    mutable size_t           hunting_made_ = 0;
    mutable std::map<std::string, OpenTemplate> made_;     // by archetype path (stable addresses)
    mutable std::set<uint64_t>                  minted_;   // the static NetGUIDs handed out
};

/// A NetGUID ObjectId for the static object at `path`: a hash of it with the low bit set (retail's static ids are odd), moved on while `taken` says
/// it is in use.  Exposed for the tests.
uint64_t static_object_id(const std::string& path, const std::function<bool(uint64_t)>& taken);

}  // namespace aoc::worldd::spawns

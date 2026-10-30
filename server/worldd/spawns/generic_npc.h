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
//      * UPackageMapClient::InternalLoadObject reads the export's NetworkChecksum when the export flags have bit 2
//        (the "%s has network checksum %u" log) into a local and skips the whole check when it is 0
//        ("Default object package network checksum mismatch!" is logged only past that skip, and only when the
//        loaded object's checksum differs);
//      * the guid cache's own check (it logs "GetObjectFromNetGUID: Network checksum mismatch" and closes the
//        connection with ENetCloseResult::NetChecksumMismatch) first tests the cache
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
//  People.  A humanoid Blueprint (the player's master mesh, whose body is merged from an NPCAppearanceComponent) is made from a recorded person, not the Wolf: a villager's open
//  (its own stats, nothing to say) or, for a point whose instance or asset has a conversation, a shop or an interaction, a citizen's with the interactable subobject.  The
//  component's content is the donor's until spawns/npc_looks.h dresses the NPC.
//
//  What a person who talks offers.  The recorded citizen is, in the game, a Construction Crate Vendor: its interactable carries that vendor's component and its one input, "sell a crate"
//  ("You must have a construction crate to sell to this vendor"), which the client always shows.  A person made from it would offer to buy crates whatever he is, so the interactable of
//  a person who talks is replaced by that of a plain conversation, taken from another recorded open: the component whose one input opens the dialogue.  The rest of the open is the
//  citizen's.
//
//  The same goes for a captured open itself.  A Blueprint a capture opened is used as it was recorded, and the Kaelar_Male_Citizen (the Construction Crate Vendor) and Kaelar_Male_Merchant (a
//  Citizen Commodities vendor) opens were recorded on crate buyers: every other NPC made from either Blueprint (the market's and the town hall's vendors, the plain citizens) would offer to buy
//  crates too.  `without_crate_sale` gives such an NPC the same open with the interactable of a plain conversation, unless the data names the very component the open was recorded from.
//
//  Humanoids that fight.  A hostile humanoid whose fights are built (fighting_humanoid: the bandit raiders and captains) is made like any person, but with the ability component of a recorded
//  guard: a villager's ability component carries no list of records, so the client would have no record of the abilities the creature is told to play, and a guard's does.  The guard's list is
//  replaced by the abilities of the Blueprint's own state tree (GenericConfig::abilities_of), which is what the game's server lists; nothing else of the guard's open is taken (its emotes and
//  targets are the guard's own).
//
//  Creatures that fight.  A creature whose Blueprint the creature tables list (the abilities of its state tree: GenericConfig::abilities_of) has those abilities as the records of the
//  plain donor's ability component, in place of the donor's own: the client plays a montage of an ability only when it holds the record, so a goblin made from the Wolf's open with
//  the Wolf's records would never animate its attacks.
//
//  Huntable creatures (Gathering stage 3, "Hunting"): an asset whose PlayerInteractionDef is one of the Hunting_<kind> defs (GenericConfig::hunting_defs) gets an eighth block, a
//  UInteractableComponent, as the living Hunting_Raven and Hunting_Spider of the captures have (the retail capture packet 1561, 758 packet 4538, and a Tidesnapper's in 1458): the Raven's own block
//  with the asset's two ids put in (ComponentDefId = the asset's InteractComponent, InteractionInputData[0].PlayerInteractDefinition = its InteractionDef).  The three captured blocks differ
//  in those two ids alone, which tools/capture/gen_hunting.py and test_worldd_hunting prove to the bit.  Every other creature keeps its seven blocks.
//
//  A huntable's other blocks are a living huntable's too, not a hostile creature's (the same Pop_Raven instance of the retail capture sends both): its CombatInfo writes bIsDead, bInvulnerable and
//  bNotTargetable (all 0), its AbilityComponent is empty (it fights nothing), its UNPCInfo carries the CharacterName the nameplate shows (a hostile creature's open has none: the client reads
//  the Blueprint's), and its one Blueprint alignment is the Friendly one (npc_motion.tsv: 6064632310208593979, Friendly to the Player alignment in alignment_relation), where the hostile
//  Raven lists two hostile ones.  The first two are copied from the captured living Raven, the name is put in, and the alignment comes from the Blueprint as for every creature.
//
//  A Blueprint that is not a pawn (npc_motion.tsv has no line for it: a banner, a tent, a quest sparkle), an excluded one, and every Blueprint when
//  the table is not loaded or the switch is off, get no template, and the loader counts them by name.
//
//  Stations (crafting, step 0).  A crafting or processing station is no pawn (it has no line in npc_motion.tsv) and is made the same way from a captured station of the same
//  native class: the Apprentice station of a processing (or a fast-craft) family is the donor, the Blueprint's path comes from the data, the checksum is 0 and the NetGUIDs are
//  worldd's own.  What a station's open holds (the actor, its InteractableComponent, NodePlotComponent and MapRegistrationComponentBP) is the same for every Blueprint of the
//  family; what is the Blueprint's own (its meshes, its window's recipes) is in the client's copy of the Blueprint, named by the path.  The Novice stations of a Camp are made so:
//  no capture opened them (the one recorded open of a Novice station has the same four blocks as the Apprentice ones).
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
    /// Quests part 3, slice 1: the interactable definitions of the quest objects (content::ObjectData).  A Blueprint that is no pawn and whose instance names one of them is a quest object
    /// (a body, a crate, a root, a building site): it is made from a recorded one.  Empty: no quest object is made.
    std::set<int64_t>        prop_components;
    /// The ability records of the Blueprint with this archetype path: the abilities of its state tree, which its open lists as the ones its AbilityComponent loads (the client plays only the
    /// abilities it has the record of).  Empty (or null): the Blueprint has none of its own to list, and a hostile humanoid is then not made from a guard's open (see fighting_humanoid).
    std::function<std::vector<int64_t>(const std::string& archetype_path)> abilities_of;
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

/// Whether the Blueprint is a hostile humanoid whose fights are built: the bandit raiders and captains.  They are dressed (spawns/npc_looks.h) and opened with the ability component of a recorded
/// guard, whose list of records holds the abilities of their own state tree; every other hostile humanoid stays out of the world until its fights are built.
bool fighting_humanoid(const std::string& archetype_or_class);

class GenericNpcs {
public:
    /// `blueprints` (may be null) must outlive this.
    GenericNpcs(const CapturedData& captured, const npc::MotionTable* blueprints, GenericConfig config);

    /// The open of an NPC of `asset`'s Blueprint (a captured template is the caller's first choice); nullptr with `why` when there is none.  A huntable creature's has the eighth
    /// block (above).  The pointer is valid as long as this object.
    /// `talks`: the point has something to say or sell (NpcLooks::talks), which a humanoid's open then carries the interactable subobject for.
    const OpenTemplate* template_for(const content::NpcAssetRow& asset, Verdict* why = nullptr, bool talks = false) const;
    /// The open of a station (see above): nullptr with `why` when the captures hold no station of the asset's family.
    const OpenTemplate* station_template(const content::NpcAssetRow& asset, Verdict* why = nullptr) const;
    /// Quests part 3, slice 1: is `component` the interactable definition of a quest object?
    bool is_prop_component(int64_t component) const { return component != 0 && config_.prop_components.count(component) != 0; }
    /// The open of a quest object: a recorded one (category "prop", an actor and its interactable component) with this Blueprint's path in place of the recorded one's; the instance's
    /// definitions are put in when it is made (spawns::instantiate).  nullptr with `why` when the captures hold none.  The pointer is valid as long as this object.
    const OpenTemplate* prop_template(const content::NpcAssetRow& asset, Verdict* why = nullptr) const;
    size_t props_made() const { return props_made_; }

    /// A captured open for an NPC whose data does not make it the crate seller the open was recorded from: `open` itself when its interactable offers no crate sale, when the data names its
    /// component (the instance's InteractComponentDefId, else the asset's), or when the captures hold no plain conversation; else a copy with the interactable of a plain conversation.
    /// The copy is kept: the pointer is valid as long as this object.
    const OpenTemplate* without_crate_sale(const OpenTemplate& open, const content::NpcAssetRow& asset, const content::NpcInstanceRow* instance) const;
    /// How many such copies were made (Blueprints, not NPCs).
    size_t conversations_made() const { return conversations_made_; }

    /// What the donor is (its archetype path), "" when the captures have none.
    const std::string& donor() const { return donor_path_; }
    /// The archetype whose recorded interactable (a plain conversation) the people who talk are given; "" when the captures hold none.
    const std::string& talker_interactable() const { return dialogue_path_; }
    const GenericConfig& config() const { return config_; }
    size_t made() const { return made_.size() - hunting_made_ - stations_made_ - props_made_; }   // the Blueprints' plain templates
    /// How many of the templates made are a station's (crafting, step 0).
    size_t stations_made() const { return stations_made_; }
    /// How many of the templates made are a huntable creature's (eight blocks).
    size_t hunting_made() const { return hunting_made_; }
    /// Whether the captures hold the living huntable the eighth block is made from (the Hunting_Raven's open).
    bool has_hunting_block() const { return hunt_block_ != nullptr; }

private:
    /// The plain template of the asset's Blueprint (seven blocks), made and kept; nullptr with `why` when the Blueprint has none.
    const OpenTemplate* make_plain(const content::NpcAssetRow& asset, Verdict* why, bool talks) const;
    /// The archetype's export of a Blueprint path (the class default object and its package, with NetGUIDs of worldd's own), minted on the first request and the same for every
    /// template of that path: a humanoid has two (the silent one and the one with the interactable subobject), and one Blueprint must not be exported under two NetGUIDs.
    /// nullptr when the path names no package.
    const repl::ExportChain* archetype_export(const std::string& path) const;

    const CapturedData*      captured_;
    const npc::MotionTable*  blueprints_;
    GenericConfig            config_;
    std::set<std::string>    excluded_;      // lower case
    const OpenTemplate*      donor_ = nullptr;
    std::string              donor_path_;
    const OpenTemplate*      person_ = nullptr;       // a recorded humanoid with the appearance component and nothing to say (the donor of a humanoid Blueprint no capture opened)
    const OpenTemplate*      talker_ = nullptr;       // ... and one with the interactable subobject as well
    const ParsedBlock*       dialogue_ = nullptr;     // the UInteractableComponent of a plain conversation: its only input opens the dialogue (what a person who talks is given)
    std::string              dialogue_path_;          // ... and the archetype it was recorded from
    const OpenTemplate*      fighter_ = nullptr;      // a recorded guard: a person whose ability component carries records (the donor of that component for a humanoid that fights)
    const ParsedBlock*       hunt_block_ = nullptr;   // the living Hunting_Raven's UInteractableComponent
    const OpenTemplate*      living_ = nullptr;       // ... and its whole open (its CombatInfo and AbilityComponent are every huntable's)
    mutable size_t           hunting_made_ = 0;
    mutable size_t           conversations_made_ = 0;
    mutable size_t           stations_made_ = 0;
    mutable size_t           props_made_ = 0;
    mutable std::map<std::string, OpenTemplate> made_;     // by archetype path (stable addresses)
    mutable std::set<uint64_t>                  minted_;   // the static NetGUIDs handed out
    mutable std::map<std::string, repl::ExportChain> exports_;   // the archetype exports, by archetype path (stable addresses)
};

/// A NetGUID ObjectId for the static object at `path`: a hash of it with the low bit set (retail's static ids are odd), moved on while `taken` says
/// it is in use.  Exposed for the tests.
uint64_t static_object_id(const std::string& path, const std::function<bool(uint64_t)>& taken);

}  // namespace aoc::worldd::spawns

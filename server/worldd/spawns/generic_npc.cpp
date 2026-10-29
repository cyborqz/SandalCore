// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/generic_npc.cpp
//
//  See generic_npc.h.
// ============================================================================
#include "spawns/generic_npc.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>

namespace aoc::worldd::spawns {

using protocol::emit::FIntrepidNetworkGUID;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}
// C O A x Cyb

uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// A station Blueprint: under /Artisanship/, a CraftingStation_* or ProcessingStation_* class.
bool is_station_path(const std::string& path) {
    if (path.find("/Artisanship/") == std::string::npos) return false;
    const std::string leaf = blueprint_leaf(path);
    return leaf.rfind("CraftingStation_", 0) == 0 || leaf.rfind("ProcessingStation_", 0) == 0;
}

/// The stock interaction that opens a conversation, and the component definition whose only input it is.
constexpr int64_t kOpenDialogueInteraction = 6064633690231341063ll;
constexpr int64_t kConversationComponent = 6064633962143220744ll;

/// The interactions that sell a crate to a vendor: a construction crate, a crate of each commodity kind (citizen, guild, harbor, market, node, smugglers, starter), a sport fishing crate, and
/// the generic "sell a crate".
constexpr int64_t kCrateSaleInteractions[] = {6064634121657450668ll, 6064634103382540406ll, 6064634103391256695ll, 6064634103400169592ll, 6064634103419175033ll,
                                              6064634103422255226ll, 6064634103425269883ll, 6064634694382323005ll, 6064634522698383388ll, 6064633962275930123ll};

/// Whether an interactable subobject has an input that sells a crate.
bool offers_crate_sale(const ParsedBlock& b) {
    if (!b.schema) return false;
    const repl::Value* data = b.state.state.get(*b.schema, "InteractionInputData");
    const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
    if (!arr) return false;
    for (const auto& element : arr->elements) {
        const auto g = element.values.find(6);   // InteractionInputData.PlayerInteractDefinition.Guid
        const auto* guid = g == element.values.end() ? nullptr : g->second.get_if<int64_t>();
        if (guid && std::find(std::begin(kCrateSaleInteractions), std::end(kCrateSaleInteractions), *guid) != std::end(kCrateSaleInteractions)) return true;
    }
    return false;
}

/// Whether an interactable subobject is a plain conversation: its first input is the stock open-dialogue interaction and no other input names an interaction.  A component with
/// more inputs (a vendor's shop, the sale of a crate) is not.
bool is_plain_conversation(const ParsedBlock& b) {
    if (!b.schema) return false;
    const repl::Value* data = b.state.state.get(*b.schema, "InteractionInputData");
    const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
    if (!arr || arr->elements.empty()) return false;
    for (size_t i = 0; i < arr->elements.size(); ++i) {
        const auto g = arr->elements[i].values.find(6);   // InteractionInputData.PlayerInteractDefinition.Guid
        const auto* guid = g == arr->elements[i].values.end() ? nullptr : g->second.get_if<int64_t>();
        const int64_t value = guid ? *guid : 0;
        if (i == 0 ? value != kOpenDialogueInteraction : value != 0) return false;
    }
    return true;
}

/// The block classes of a plain creature's open, in the order retail sends them.
const char* const kPlain[] = {"ANPCCharacter", "UAlignmentComponent", "UNPCSecondaryInfo", "UNPCInfo", "UNPCCombatInfo", "UAoCAbilityComponent", "UAoCStatsComponent"};

bool is_plain(const OpenTemplate& t) {
    if (t.category != "npc" || t.blocks.size() != std::size(kPlain)) return false;
    for (size_t i = 0; i < t.blocks.size(); ++i) {
        if (t.blocks[i].cls != kPlain[i]) return false;
    }
    return true;
}

/// A recorded person: the plain creature's seven blocks, the interactable subobject when `talks`, then the appearance component.
bool is_person(const OpenTemplate& t, bool talks) {
    if (t.category != "npc" || t.blocks.size() != std::size(kPlain) + (talks ? 2u : 1u)) return false;
    for (size_t i = 0; i < std::size(kPlain); ++i) {
        if (t.blocks[i].cls != kPlain[i]) return false;
    }
    if (talks && t.blocks[std::size(kPlain)].cls != "UInteractableComponent") return false;
    return t.blocks.back().cls == "UNPCAppearanceComponent";
}

/// The hostile humanoids whose fights are built, by the Blueprint's class name: the bandit raiders (a two-handed sword: strikes, a cleave, a heavy slash, a whirlwind, a leap) and the bandit
/// captains (a sword and shield).  The camps they stand in are mixed with archers, mages and healers whose arrows, bolts and heals are not built yet: those stay out of the world.
constexpr const char* kFightingHumanoids[] = {
    "Bandit_Raider", "Bandit_Raider1", "Bandit_Raider2", "Bandit_Raider3", "CSB_Bandit_Raider_StateTree",
    "Bandit_Raider_RareSpawn_BigBrother", "Bandit_Raider_RareSpawn_RiverRunnerRiggs", "BanditRaider_RareSpawn_VanguardRowan",
    "Bandit_Captain", "Bandit_Captain1", "Bandit_Captain2", "CSB_Forsaken_Blades_Sergeant",
    "Bandit_Captain_Clang", "Bandit_Captain_Capn_Knuckles", "Bandit_Captain_RareSpawn_TheShotcaller", "Bandit_Captain_RareSpawn_ChiefArmorerJannus"};

/// Gives the template the ability component of the recorded guard `guard` (a component with a list of records, which a villager's lacks) and puts `kit` into it as the records it loads
/// (ReplicatedRecordsToLoad), each element made like the first of the guard's own list.  Everything else of the template stays: only this component comes from the guard.  False when
/// either has no ability component or the guard's list is empty (there is no element to copy the shape of).
bool adopt_ability_records(OpenTemplate& t, const OpenTemplate& guard, const std::vector<int64_t>& kit) {
    const int to = t.find("UAoCAbilityComponent"), from = guard.find("UAoCAbilityComponent");
    if (to <= 0 || from <= 0) return false;
    ParsedBlock& block = t.blocks[static_cast<size_t>(to)];
    const ParsedBlock& donor = guard.blocks[static_cast<size_t>(from)];
    if (!block.schema || block.schema != donor.schema) return false;
    block.state = donor.state;
    const repl::Value* v = block.state.state.get(*block.schema, "ReplicatedRecordsToLoad");
    const auto* list = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.empty()) return false;
    repl::ArrayValue made;
    for (const int64_t guid : kit) {
        repl::ElementState e = list->elements.front();
        e.values[1] = repl::Value::i64(guid);
        made.elements.push_back(std::move(e));
    }
    return block.state.state.set(*block.schema, "ReplicatedRecordsToLoad", repl::Value(std::move(made)));
}

}  // namespace

bool fighting_humanoid(const std::string& archetype_or_class) {
    const std::string leaf = blueprint_leaf(archetype_or_class);
    for (const char* name : kFightingHumanoids) {
        if (leaf == name) return true;
    }
    return false;
}

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Generic: return "generic";
        case Verdict::NotPawn: return "not a pawn";
        case Verdict::Excluded: return "excluded";
        case Verdict::Disabled: return "switched off";
        case Verdict::NoTable: return "no npc_motion.tsv";
        case Verdict::NoDonor: return "no donor open";
        case Verdict::NoPath: return "no blueprint";
    }
    return "?";
}

std::string blueprint_leaf(const std::string& s) {
    const size_t dot = s.rfind('.');
    std::string leaf = s.substr(dot == std::string::npos ? (s.rfind('/') == std::string::npos ? 0 : s.rfind('/') + 1) : dot + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    return leaf;
}

uint64_t static_object_id(const std::string& path, const std::function<bool(uint64_t)>& taken) {
    uint64_t salt = 0;
    for (;;) {
        const uint64_t id = (mix(fnv1a(path) ^ (salt * 0xD6E8FEB86659FD93ull)) | 1ull);
        if (!taken || !taken(id)) return id;
        ++salt;
    }
}

GenericNpcs::GenericNpcs(const CapturedData& captured, const npc::MotionTable* blueprints, GenericConfig config)
    : captured_(&captured), blueprints_(blueprints), config_(std::move(config)) {
    for (const std::string& e : config_.exclude) {
        if (!e.empty()) excluded_.insert(lower(e.find('/') == std::string::npos ? blueprint_leaf(e) : e));
    }
    // the plain donor: the Wolf's open when the captures have it, else the first plain creature in path order
    const OpenTemplate* wolf = nullptr;
    const OpenTemplate* first = nullptr;
    for (const OpenTemplate& t : captured.opens()) {
        if (!is_plain(t)) continue;
        if (t.archetype_path.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf = &t;
        if (!first || t.archetype_path < first->archetype_path) first = &t;
    }
    donor_ = wolf ? wolf : first;
    if (donor_) donor_path_ = donor_->archetype_path;
    // the people's donors: a villager's open and a citizen's (the first in path order when the preferred one is not there)
    for (const OpenTemplate& t : captured.opens()) {
        const bool villager = blueprint_leaf(t.archetype_path) == "Node_Villager_M", citizen = blueprint_leaf(t.archetype_path) == "Kaelar_Male_Citizen";
        if (is_person(t, false) && (!person_ || villager || (person_->archetype_path.find("Node_Villager_M.") == std::string::npos && t.archetype_path < person_->archetype_path))) person_ = &t;
        if (is_person(t, true) && (!talker_ || citizen || (talker_->archetype_path.find("Kaelar_Male_Citizen.") == std::string::npos && t.archetype_path < talker_->archetype_path))) talker_ = &t;
    }
    // the interactable of a plain conversation, for the people who talk: the component the data names for it first, else any recorded one whose only input opens the dialogue
    for (const bool preferred : {true, false}) {
        for (const OpenTemplate& t : captured.opens()) {
            if (dialogue_ || t.category != "npc") continue;
            const int i = t.find("UInteractableComponent");
            if (i <= 0 || !is_plain_conversation(t.blocks[static_cast<size_t>(i)])) continue;
            const ParsedBlock& b = t.blocks[static_cast<size_t>(i)];
            const repl::Value* def = b.state.state.get(*b.schema, "ComponentDefId.Guid");
            const auto* guid = def ? def->get_if<int64_t>() : nullptr;
            if (preferred && !(guid && *guid == kConversationComponent)) continue;
            dialogue_ = &b;
            dialogue_path_ = t.archetype_path;
        }
    }
    // the donor of a humanoid that fights: a recorded guard (a person whose ability component carries records), the Soldier when the captures have it
    for (const OpenTemplate& t : captured.opens()) {
        const std::string leaf = blueprint_leaf(t.archetype_path);
        if (leaf.rfind("Guard_", 0) != 0 || !is_person(t, false)) continue;
        if (!fighter_ || leaf == "Guard_Soldier" || (blueprint_leaf(fighter_->archetype_path) != "Guard_Soldier" && t.archetype_path < fighter_->archetype_path)) fighter_ = &t;
    }
    // the living huntable's eighth block: the Hunting_Raven's open, whose interactable the other kinds' differ from in two ids only
    for (const OpenTemplate& t : captured.opens()) {
        if (t.category != "npc" || t.archetype_path.find("/Hunting_Raven.Default__Hunting_Raven_C") == std::string::npos) continue;
        const int i = t.find("UInteractableComponent");
        if (i > 0 && t.blocks.size() == 8) {
            hunt_block_ = &t.blocks[static_cast<size_t>(i)];
            living_ = &t;
        }
    }
}

const OpenTemplate* GenericNpcs::template_for(const content::NpcAssetRow& asset, Verdict* why, bool talks) const {
    const auto fail = [&](Verdict v) -> const OpenTemplate* {
        if (why) *why = v;
        return nullptr;
    };
    if (why) *why = Verdict::Generic;
    const std::string& path = asset.archetype_path;
    if (path.empty()) return fail(Verdict::NoPath);
    if (!config_.enabled) return fail(Verdict::Disabled);
    if (is_station_path(path)) {
        if (!excluded_.empty() && (excluded_.count(lower(blueprint_leaf(path))) || excluded_.count(lower(path)))) return fail(Verdict::Excluded);
        return station_template(asset, why);
    }
    if (!blueprints_ || blueprints_->empty()) return fail(Verdict::NoTable);
    if (!excluded_.empty() && (excluded_.count(lower(blueprint_leaf(path))) || excluded_.count(lower(path)))) return fail(Verdict::Excluded);
    const OpenTemplate* plain = nullptr;
    // a person who has something to say or sell has its own template (the one with the interactable subobject), kept beside the silent one
    const auto motion_row = blueprints_->find(path);
    const bool person_talks = talks && motion_row && motion_row->appearance_dependent();
    const std::string plain_key = person_talks ? path + "#talks" : path;
    if (const auto it = made_.find(plain_key); it != made_.end()) plain = &it->second;
    if (!plain) plain = make_plain(asset, why, person_talks);
    if (!plain) return nullptr;
    // a huntable creature: the plain template with the eighth block (the template is kept per interaction: two assets may share one Blueprint, HuntingTest and Hunting_Wolf do)
    if (hunt_block_ && asset.interaction_def != 0 && asset.interact_component != 0 && config_.hunting_defs.count(asset.interaction_def)) {
        const std::string key = path + "#hunting#" + std::to_string(asset.interact_component) + "#" + std::to_string(asset.interaction_def);
        if (const auto it = made_.find(key); it != made_.end()) return &it->second;
        OpenTemplate t = *plain;
        // a living huntable's other blocks (the retail capture's Raven, to the bit in test_worldd_hunting): the CombatInfo's three flags written, the AbilityComponent empty, the nameplate's name
        for (const char* cls : {"UNPCCombatInfo", "UAoCAbilityComponent"}) {
            const int to = t.find(cls), from = living_ ? living_->find(cls) : -1;
            if (to > 0 && from > 0) t.blocks[static_cast<size_t>(to)].state = living_->blocks[static_cast<size_t>(from)].state;
        }
        if (const auto name = config_.hunting_names.find(asset.interaction_def); name != config_.hunting_names.end()) {
            if (const int ni = t.find("UNPCInfo"); ni > 0) {
                ParsedBlock& info = t.blocks[static_cast<size_t>(ni)];
                if (info.schema) info.state.state.set(*info.schema, "CharacterName", repl::Value::str(name->second));
            }
        }
        ParsedBlock block = *hunt_block_;
        const bool ok = block.schema && block.state.state.set(*block.schema, "ComponentDefId.Guid", repl::Value::i64(asset.interact_component));
        const repl::Value* data = ok ? block.state.state.get(*block.schema, "InteractionInputData") : nullptr;
        const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
        if (!arr || arr->elements.empty()) return plain;   // (the captured block is not what it was read as: the creature stays plain)
        repl::ArrayValue copy = *arr;
        copy.elements[0].values[6] = repl::Value::i64(asset.interaction_def);   // InteractionInputData.PlayerInteractDefinition.Guid
        if (!block.state.state.set(*block.schema, "InteractionInputData", repl::Value(std::move(copy)))) return plain;
        t.blocks.push_back(std::move(block));
        ++hunting_made_;
        return &made_.emplace(key, std::move(t)).first->second;
    }
    return plain;
}

const OpenTemplate* GenericNpcs::without_crate_sale(const OpenTemplate& open, const content::NpcAssetRow& asset, const content::NpcInstanceRow* instance) const {
    if (!dialogue_) return &open;
    const int at = open.find("UInteractableComponent");
    if (at <= 0) return &open;
    const ParsedBlock& block = open.blocks[static_cast<size_t>(at)];
    if (!offers_crate_sale(block)) return &open;
    // the NPC is the seller the open was recorded from when its data names that very component
    const repl::Value* def = block.state.state.get(*block.schema, "ComponentDefId.Guid");
    const auto* guid = def ? def->get_if<int64_t>() : nullptr;
    const int64_t own = instance && instance->interact_component != 0 ? instance->interact_component : asset.interact_component;
    if (guid && *guid != 0 && own == *guid) return &open;
    const std::string key = open.archetype_path + "#conversation";
    if (const auto it = made_.find(key); it != made_.end()) return &it->second;
    OpenTemplate t = open;
    t.blocks[static_cast<size_t>(at)] = *dialogue_;
    ++conversations_made_;
    return &made_.emplace(key, std::move(t)).first->second;
}

const repl::ExportChain* GenericNpcs::archetype_export(const std::string& path) const {
    if (const auto it = exports_.find(path); it != exports_.end()) return &it->second;
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return nullptr;
    const std::string package = path.substr(0, dot), object = path.substr(dot + 1);
    const auto taken = [&](uint64_t id) { return captured_->static_id_in_use(id) || minted_.count(id) != 0; };
    repl::ExportLink obj, pkg;
    obj.guid = FIntrepidNetworkGUID::from_u64(static_object_id(path, taken));
    minted_.insert(obj.guid.ObjectId);
    obj.name = object;
    obj.has_checksum = true;
    obj.checksum = config_.export_checksum;
    pkg.guid = FIntrepidNetworkGUID::from_u64(static_object_id(package + "#package", taken));
    minted_.insert(pkg.guid.ObjectId);
    pkg.name = package;
    pkg.has_checksum = true;
    pkg.checksum = 0;
    return &exports_.emplace(path, repl::ExportChain{obj, pkg}).first->second;
}

const OpenTemplate* GenericNpcs::station_template(const content::NpcAssetRow& asset, Verdict* why) const {
    const auto fail = [&](Verdict v) -> const OpenTemplate* {
        if (why) *why = v;
        return nullptr;
    };
    if (why) *why = Verdict::Generic;
    const std::string& path = asset.archetype_path;
    if (!is_station_path(path)) return fail(Verdict::NotPawn);
    if (const auto it = made_.find(path); it != made_.end()) return &it->second;
    // the donor: a captured Apprentice station of the same family (processing or fast-craft), the first in the tables' order
    const bool processing = blueprint_leaf(path).rfind("ProcessingStation_", 0) == 0;
    const OpenTemplate* donor = nullptr;
    for (const OpenTemplate& t : captured_->opens()) {
        if (t.category != "station" || t.blocks.size() != 4) continue;
        const std::string leaf = blueprint_leaf(t.archetype_path);
        if ((leaf.rfind("ProcessingStation_", 0) == 0) != processing) continue;
        if (leaf.size() < 3 || leaf.compare(leaf.size() - 3, 3, "_T2") != 0) continue;
        donor = &t;
        break;
    }
    if (!donor) return fail(Verdict::NoDonor);
    OpenTemplate t = *donor;
    t.archetype_path = path;
    t.capture = t.channel = t.packet = 0;   // made here: no capture opened it
    const repl::ExportChain* exported = archetype_export(path);
    if (!exported) return fail(Verdict::NoPath);
    t.archetype = *exported;
    ++stations_made_;
    return &made_.emplace(path, std::move(t)).first->second;
}

const OpenTemplate* GenericNpcs::make_plain(const content::NpcAssetRow& asset, Verdict* why, bool talks) const {
    const auto fail = [&](Verdict v) -> const OpenTemplate* {
        if (why) *why = v;
        return nullptr;
    };
    const std::string& path = asset.archetype_path;
    const auto row = blueprints_->find(path);
    if (!row) return fail(Verdict::NotPawn);
    // a humanoid is made from a recorded person (its body is merged from the appearance component the Wolf's open does not carry), any other Blueprint from the plain donor; a hostile humanoid
    // whose fights are built has the ability component of a recorded guard besides, with the abilities of its tree in the list of records that component carries
    const bool person = row->appearance_dependent();
    std::vector<int64_t> kit;
    if (person && fighter_ && config_.abilities_of && fighting_humanoid(path)) kit = config_.abilities_of(path);
    const bool fights = !kit.empty();
    // a creature whose fights are built (the creature tables list the abilities of its tree) lists them in the plain donor's ability component, as the game's server does for each Blueprint: with the
    // donor's own list the client has no record of the abilities the creature is told to play, and the creature keeps its single swing
    std::vector<int64_t> creature_kit;
    if (!person && donor_ && config_.abilities_of) creature_kit = config_.abilities_of(path);
    const OpenTemplate* donor = person ? (talks && talker_ ? talker_ : person_) : donor_;
    if (!donor) return fail(Verdict::NoDonor);
// [CoA ~ C:Y:B]

    OpenTemplate t = *donor;
    t.archetype_path = path;
    t.capture = t.channel = t.packet = 0;   // made here: no capture opened it
    // a person who talks offers a conversation, not the sale of a crate (the recorded citizen was a Construction Crate Vendor: see the top of the header)
    if (person && talks && talker_ && dialogue_) {
        const int at = t.find("UInteractableComponent");
        if (at > 0) t.blocks[static_cast<size_t>(at)] = *dialogue_;
    }
    // the archetype's export: the class default object in its package, checksum 0 (see the header), static NetGUIDs of worldd's own, the same for each template of the path
    const repl::ExportChain* exported = archetype_export(path);
    if (!exported) return fail(Verdict::NoPath);
    t.archetype = *exported;
    // the Blueprint's scale (an NPC carries it in SerializeNewActor; 1 is the absence of one)
    t.spawn.scale.reset();
    if (row->default_scale != 1.0) {
        const int64_t bits = static_cast<int64_t>(std::bit_cast<uint64_t>(row->default_scale));
        t.spawn.scale = repl::QuantVec{repl::QuantVec::Mode::RawDouble, bits, bits, bits};
    }
    // the alignments: the Blueprint's StartingAlignments, then the creature's own (the instigator record with its CharacterGuid), as retail lists them
    for (ParsedBlock& b : t.blocks) {
        if (b.cls != "UAlignmentComponent" || !b.schema || row->alignments.empty()) continue;
        const repl::Value* v = b.state.state.get(*b.schema, "Alignments");
        const auto* c = v ? v->get_if<repl::AlignmentContainer>() : nullptr;
        if (!c) continue;
        repl::AlignmentContainer made;
        for (const int64_t a : row->alignments) {
            repl::AlignmentContainer::Instance inst;
            inst.key = static_cast<uint64_t>(a);
            inst.guids.push_back(repl::GuidValue{});
            made.instances.push_back(std::move(inst));
        }
        for (const auto& inst : c->instances) {
            const bool own = std::any_of(inst.guids.begin(), inst.guids.end(), [](const repl::GuidValue& g) { return (g.a | g.b | g.c | g.d) != 0; });
            if (own) made.instances.push_back(inst);   // the donor's instigator record: instantiate gives it the new CharacterGuid
        }
        made.timers = c->timers;
        b.state.state.set(*b.schema, "Alignments", repl::Value(std::move(made)));
    }
    if (fights && !adopt_ability_records(t, *fighter_, kit)) return fail(Verdict::NoDonor);   // (the guard's ability component is not what it was read as: the humanoid is not made at all)
    if (!creature_kit.empty()) adopt_ability_records(t, *donor_, creature_kit);   // (a donor whose list is empty, or another component, keeps its own list: the creature then fights with its single swing)
    if (fights) {
        // the villager's emote is not a bandit's: the game's open for a raider sets only the look, the population ids and the pose in its secondary info
        const int si = t.find("UNPCSecondaryInfo");
        const auto* emote = si > 0 && t.blocks[static_cast<size_t>(si)].schema ? t.blocks[static_cast<size_t>(si)].schema->command("EmoteAnimationInfoGuid") : nullptr;
        if (emote) t.blocks[static_cast<size_t>(si)].state.state.erase(emote->handle);
    }
    return &made_.emplace(person && talks ? path + "#talks" : path, std::move(t)).first->second;
}

}  // namespace aoc::worldd::spawns

// the people's looks and names (spawns/npc_looks.h) are compiled here: see the note at the top of that file
#include "spawns/npc_looks.cpp"

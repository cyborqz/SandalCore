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

/// The block classes of a plain creature's open, in the order retail sends them.
const char* const kPlain[] = {"ANPCCharacter", "UAlignmentComponent", "UNPCSecondaryInfo", "UNPCInfo", "UNPCCombatInfo", "UAoCAbilityComponent", "UAoCStatsComponent"};

bool is_plain(const OpenTemplate& t) {
    if (t.category != "npc" || t.blocks.size() != std::size(kPlain)) return false;
    for (size_t i = 0; i < t.blocks.size(); ++i) {
        if (t.blocks[i].cls != kPlain[i]) return false;
    }
    return true;
}

}  // namespace

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
}

const OpenTemplate* GenericNpcs::template_for(const content::NpcAssetRow& asset, Verdict* why) const {
    const auto fail = [&](Verdict v) -> const OpenTemplate* {
        if (why) *why = v;
        return nullptr;
    };
    if (why) *why = Verdict::Generic;
    const std::string& path = asset.archetype_path;
    if (path.empty()) return fail(Verdict::NoPath);
    if (!config_.enabled) return fail(Verdict::Disabled);
    if (!blueprints_ || blueprints_->empty()) return fail(Verdict::NoTable);
    if (!excluded_.empty() && (excluded_.count(lower(blueprint_leaf(path))) || excluded_.count(lower(path)))) return fail(Verdict::Excluded);
    if (const auto it = made_.find(path); it != made_.end()) return &it->second;
    const auto row = blueprints_->find(path);
    if (!row) return fail(Verdict::NotPawn);
    if (!donor_) return fail(Verdict::NoDonor);
// [CoA ~ C:Y:B]

    OpenTemplate t = *donor_;
    t.archetype_path = path;
    t.capture = t.channel = t.packet = 0;   // made here: no capture opened it
    // the archetype's export: the class default object in its package, checksum 0 (see the header), static NetGUIDs of worldd's own
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return fail(Verdict::NoPath);
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
    t.archetype = {obj, pkg};
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
    return &made_.emplace(path, std::move(t)).first->second;
}

}  // namespace aoc::worldd::spawns

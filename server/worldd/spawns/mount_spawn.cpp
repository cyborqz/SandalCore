// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/mount_spawn.cpp
//
//  See mount_spawn.h.
// ============================================================================
#include "spawns/mount_spawn.h"

#include "bootstrap/constants.h"
#include "spawns/generic_npc.h"
#include "spawns/spawn_util.h"
#include "world/interaction_state.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <sstream>

namespace aoc::worldd::spawns {

using protocol::emit::FIntrepidNetworkGUID;
using util::angle16;
using util::angle8;
using util::erase_prefix;
using util::set_path;
using util::splitmix;

namespace {

int32_t as_i32(uint32_t w) { return static_cast<int32_t>(w); }

repl::Value word_value(uint32_t w) { return repl::Value::i64(as_i32(w)); }

/// True when the value `v` is the int32 word `w` (an FGuid word is an Int32 command: held signed or, from a codec that reads it so, unsigned).
bool is_word(const repl::Value& v, uint32_t w) {
    if (const auto* i = v.get_if<int64_t>()) return static_cast<uint32_t>(*i) == w;
    if (const auto* u = v.get_if<uint64_t>()) return static_cast<uint32_t>(*u) == w;
    return false;
}

bool replace_in_value(repl::Value& v, const repl::GuidValue& from, const repl::GuidValue& to, size_t& n);

bool replace_in_element(repl::ElementState& e, const repl::GuidValue& from, const repl::GuidValue& to, size_t& n) {
    bool changed = false;
    // four consecutive ints that are the FGuid: the members A, B, C, D of a struct array's element (StateGuids)
    for (auto it = e.values.begin(); it != e.values.end(); ++it) {
        const uint16_t k = it->first;
        const auto b = e.values.find(static_cast<uint16_t>(k + 1));
        const auto c = e.values.find(static_cast<uint16_t>(k + 2));
        const auto d = e.values.find(static_cast<uint16_t>(k + 3));
        if (b == e.values.end() || c == e.values.end() || d == e.values.end()) continue;
        if (is_word(it->second, from.a) && is_word(b->second, from.b) && is_word(c->second, from.c) && is_word(d->second, from.d)) {
            it->second = word_value(to.a);
            b->second = word_value(to.b);
            c->second = word_value(to.c);
            d->second = word_value(to.d);
            ++n;
            changed = true;
            break;
        }
    }
    for (auto& [k, v] : e.values) changed = replace_in_value(v, from, to, n) || changed;
    return changed;
}
// (Cyb + C.O.A)

bool replace_in_value(repl::Value& v, const repl::GuidValue& from, const repl::GuidValue& to, size_t& n) {
    bool changed = false;
    if (auto* arr = v.get_if<repl::ArrayValue>()) {
        for (repl::ElementState& e : arr->elements) changed = replace_in_element(e, from, to, n) || changed;
    } else if (auto* al = v.get_if<repl::AlignmentContainer>()) {
        for (auto& inst : al->instances) {
            for (repl::GuidValue& g2 : inst.guids) {
                if (g2 == from) {
                    g2 = to;
                    ++n;
                    changed = true;
                }
            }
        }
    } else if (auto* w = v.get_if<repl::GuidToBoolWrapper>()) {
        for (auto& pr : w->pairs) {
            if (pr.first == from) {
                pr.first = to;
                ++n;
                changed = true;
            }
        }
    }
    return changed;
}

/// Splits "/Game/GameBlueprints/Mounts/X.X_C" into the package "/Game/GameBlueprints/Mounts/X" and the class "X_C"; false for a path without a dot.
bool split_class_path(const std::string& path, std::string& package, std::string& klass) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= path.size()) return false;
    package = path.substr(0, dot);
    klass = path.substr(dot + 1);
    return true;
}

/// The class default object's path: "<package>.Default__<class>".
std::string cdo_path(const std::string& package, const std::string& klass) { return package + ".Default__" + klass; }

repl::Subobject* find_sub(repl::Actor& a, const char* cls) {
    for (repl::Subobject& s : a.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

}  // namespace

size_t replace_guid(repl::ObjectState& o, const repl::GuidValue& from, const repl::GuidValue& to) {
    size_t n = 0;
    if (!o.schema || from == to) return 0;
    // the RepLayout values (the map is walked by handle; a value is changed in place through a copy, since RepState::set checks the value's kind)
    std::vector<std::pair<uint16_t, repl::Value>> replaced;
    for (const auto& [handle, value] : o.state.values()) {
        repl::Value copy = value;
        size_t before = n;
        replace_in_value(copy, from, to, n);
        if (n != before) replaced.emplace_back(handle, std::move(copy));
    }
    for (auto& [handle, value] : replaced) o.state.set(*o.schema, handle, std::move(value));
    // the fast arrays' items
    for (const auto& [field, array] : o.arrays.all()) {
        std::vector<std::pair<int32_t, repl::ElementState>> items;
        for (const repl::FastArrayItem& it : array.items()) {
            repl::ElementState copy = it.values;
            const size_t before = n;
            replace_in_element(copy, from, to, n);
            if (n != before) items.emplace_back(it.id, std::move(copy));
        }
        if (items.empty()) continue;
        repl::FastArrayState* a = o.arrays.get(*o.schema, field);
        for (auto& [id, values] : items) a->change(id, std::move(values));
    }
    return n;
}

std::optional<MountBook> MountBook::load(const content::ContentDb& db, std::string& error) {
    auto data = db.mount_data(error);
    if (!data) return std::nullopt;
    auto donors = MountDonors::load(error);
    if (!donors) return std::nullopt;
    return MountBook(std::move(*data), std::move(*donors));
}

bool MountBook::load_speeds(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::string line;
    std::getline(in, line);   // the header
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // review B M2: the columns are read by position, so a mapc that reorders them must be an error and not different numbers
    static const char* const kSpeedsHeader = "blueprint\tkind\tparents\tmax_walk_speed\tmax_run_speed\tmax_fly_speed\tmax_swim_speed";
    if (line.rfind(kSpeedsHeader, 0) != 0) {
        error = path + " has not the expected columns (blueprint, kind, parents, max_walk_speed, max_run_speed, max_fly_speed, max_swim_speed, ...)";
        return false;
    }
    size_t rows = 0, row_number = 1;
    while (std::getline(in, line)) {
        ++row_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) f.push_back(cell);
        if (line.back() == '\t') f.emplace_back();
        if (f.size() < 7) {
            error = path + " line " + std::to_string(row_number) + " has " + std::to_string(f.size()) + " columns (at least 7)";
            return false;
        }
        Speeds s;
        try {
            s.walk = std::stod(f[3]);
            s.run = std::stod(f[4]);
            s.fly = std::stod(f[5]);
            s.swim = std::stod(f[6]);
        } catch (const std::exception&) {
            error = path + " line " + std::to_string(row_number) + " has a speed that is not a number";
            return false;
        }
        speeds_[f[0]] = s;
        ++rows;
    }
    if (rows == 0) {
        error = path + " has no rows";
        return false;
    }
    return true;
}

const MountBook::Speeds* MountBook::speeds_of(const std::string& blueprint) const {
    const auto it = speeds_.find(blueprint);
    return it == speeds_.end() ? nullptr : &it->second;
}
// CoA : C-Y-B

bool MountBook::load_riders(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::string line;
    std::getline(in, line);   // the header: blueprint, rider_anim
    std::map<std::string, std::string> fresh;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        std::string anim = line.substr(tab + 1);
        std::string package, klass;
        if (anim.empty() || !split_class_path(anim, package, klass)) continue;   // a Blueprint with no interaction point: the kind's AnimBP
        fresh[line.substr(0, tab)] = std::move(anim);
    }
    if (fresh.empty()) {
        error = path + " names no rider AnimBP";
        return false;
    }
    riders_ = std::move(fresh);
    return true;
}

namespace {

/// The AnimBPs the retail donors named (research/mounts/evidence.md 2: the export of the InteractableComponent's AttachmentAnimation in the opens of captures 911, 898, 871).
constexpr const char* kGroundRiderAnim = "/Game/Characters/Animations/Mounts_Rider/Swiftclaw/ABP_PC_MNT_Swiftclaw.ABP_PC_MNT_Swiftclaw_C";
constexpr const char* kAquaticRiderAnim = "/Game/Characters/Animations/Mounts_Rider/Tidesnapper/ABP_PC_MNT_Tidesnapper.ABP_PC_MNT_Tidesnapper_C";
constexpr const char* kFlyingRiderAnim = "/Game/Characters/Animations/Mounts_Rider/Dawnbreaker/ABP_PC_MNT_Dawnbreaker.ABP_PC_MNT_Dawnbreaker_C";

}  // namespace

std::string MountBook::rider_anim_of(const content::MountRow& row) const {
    if (const auto it = riders_.find(row.blueprint); it != riders_.end()) return it->second;
    return row.kind == "aquatic" ? kAquaticRiderAnim : row.kind == "flying" ? kFlyingRiderAnim : kGroundRiderAnim;
}

repl::ExportChain MountBook::rider_anim_export(const std::string& class_path) const {
    std::string package, klass;
    if (!split_class_path(class_path, package, klass)) return {};
    if (const auto it = rider_exports_.find(class_path); it != rider_exports_.end()) return it->second;
    const auto taken = [&](uint64_t id) { return donors_.static_id_in_use(id) || minted_.count(id) != 0; };
    repl::ExportLink cls, pkg;
    cls.guid = FIntrepidNetworkGUID::from_u64(static_object_id(class_path, taken));
    minted_.insert(cls.guid.ObjectId);
    cls.name = klass;                // the class object: "ABP_PC_MNT_Swiftclaw_C" in its package [R: capture 911: flags 5, checksum 0]
    cls.has_checksum = true;
    cls.checksum = 0;
    pkg.guid = FIntrepidNetworkGUID::from_u64(static_object_id(package + "#package", taken));
    minted_.insert(pkg.guid.ObjectId);
    pkg.name = package;
    pkg.has_checksum = true;
    pkg.checksum = 0;
    repl::ExportChain chain{cls, pkg};
    rider_exports_[class_path] = chain;
    return chain;
}

int64_t MountBook::summon_ability() const {
    const content::MountAbilityRow* r = data_.ability_with_role("summon");
    return r ? r->ability : 0;
}

DashBar MountBook::dash_bar(const content::MountRow& row) const {
    if (row.kind == "aquatic") return {kDashAllowSwimming, 1};
    return {dash_ability(), 0};
}

int64_t MountBook::dash_ability() const {
    const content::MountAbilityRow* r = data_.ability_with_role("dash");
    return r ? r->ability : 0;
}

repl::ExportChain MountBook::archetype_of(const content::MountRow& row, const OpenTemplate& donor) const {
    std::string package, klass;
    if (!split_class_path(row.blueprint_path, package, klass)) return {};
    const std::string path = cdo_path(package, klass);
    if (path == donor.archetype_path) return donor.archetype;   // the Blueprint a capture exported keeps its retail export
    if (const auto it = made_.find(path); it != made_.end()) return it->second;
    const auto taken = [&](uint64_t id) { return donors_.static_id_in_use(id) || minted_.count(id) != 0; };
    repl::ExportLink obj, pkg;
    obj.guid = FIntrepidNetworkGUID::from_u64(static_object_id(path, taken));
    minted_.insert(obj.guid.ObjectId);
    obj.name = "Default__" + klass;
    obj.has_checksum = true;
    obj.checksum = 0;   // not compared by the client (spawns/generic_npc.h)
    pkg.guid = FIntrepidNetworkGUID::from_u64(static_object_id(package + "#package", taken));
    minted_.insert(pkg.guid.ObjectId);
    pkg.name = package;
    pkg.has_checksum = true;
    pkg.checksum = 0;
    repl::ExportChain chain{obj, pkg};
    made_[path] = chain;
    return chain;
}

double MountBook::max_health(const content::MountRow& row, int owner_level) const {
    const int level = std::clamp(owner_level, 1, 50);
    return data_.stat_at(level, content::kGuidMaxHealth, 0.0) * row.health_factor();
}

std::optional<MountMade> MountBook::make(const content::MountRow& row, const MountParams& p, const Minter& mint, std::string& error) const {
    const OpenTemplate* donor = donor_of(row);
    if (!donor) {
        error = "the captures have no " + row.kind + " mount to make " + row.item_name + " from";
        return std::nullopt;
    }
    for (const double v : {p.x, p.y, p.z, p.yaw_degrees, p.world_time}) {
        if (!std::isfinite(v)) {
            error = "a position that is not a number";
            return std::nullopt;
        }
    }
    repl::ExportChain archetype = archetype_of(row, *donor);
    if (archetype.empty()) {
        error = row.item_name + " names no Blueprint";
        return std::nullopt;
    }

    MountMade made;
    world::WorldActor& w = made.actor;
    w.kind = world::ActorKind::Mount;
    repl::Actor& a = w.actor;
    a.guid = mint();
    const FIntrepidNetworkGUID cylinder = mint();   // A+2: named by the hits and by the rider's attachment
    a.obj = donor->actor_block().state;
    a.open.archetype = std::move(archetype);
    a.open.level = bootstrap::persistent_level_export();
    a.open.spawn = donor->spawn;
    a.open.spawn.scale.reset();
    a.open.spawn.velocity.reset();
    w.id = a.guid.ObjectId;
    w.npc.archetype = donor->archetype_path;
    w.npc.collision_cylinder = cylinder.ObjectId;
    w.npc.source = world::Source::Runtime;

    // the subobjects, in the donor's order: A+4 ... A+14, then A+16 (never exported), then the mount and interactable components
    for (size_t i = 1; i < donor->blocks.size(); ++i) {
        const ParsedBlock& b = donor->blocks[i];
        repl::Subobject s;
        s.guid = mint();
        s.obj = b.state;
        if (b.kind == 1) {
            s.name = b.name;
            s.checksum = b.checksum;
        } else {
            s.dynamic_class = b.class_chain;
        }
        if (b.cls == "UInteractableComponent") w.npc.interact_component = s.guid.ObjectId;
        const bool after_stats = b.cls == "UAoCStatsComponent";
        a.subobjects.push_back(std::move(s));
        if (after_stats) (void)mint();
    }
// [C | Y | B - CoA]

    std::string e;
    repl::ObjectState& actor = a.obj;
    // where it stands, as an NPC's open has it (instantiate): ReplicatedMovement at rest, the open's own location and rotation
    {
        repl::RepMovement m;
        if (const repl::Value* v = actor.state.get(7)) {
            if (const auto* old = v->get_if<repl::RepMovement>()) m = *old;
        }
        m.simulated_physic_sleep = false;
        m.rep_physics = false;
        m.location = repl::quantize_location(actor.opts, p.x, p.y, p.z);
        m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
        m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
        m.acceleration.reset();
        m.rotation = repl::Rot{0, actor.opts.rep_movement_short_rotation ? angle16(p.yaw_degrees) : angle8(p.yaw_degrees), 0};
        if (!set_path(actor, "ReplicatedMovement", repl::Value(m), e)) { error = e; return std::nullopt; }
        a.open.spawn.location = repl::quantize_vector(10, p.x, p.y, p.z);
        a.open.spawn.rotation = repl::Rot{0, angle16(p.yaw_degrees), 0};
        w.x = p.x;
        w.y = p.y;
        w.z = p.z;
        w.yaw_degrees = p.yaw_degrees;
    }
    for (const char* prefix : {"AttachmentReplication.", "ReplicatedBasedMovement.", "RepRootMotion."}) erase_prefix(actor, prefix);
    if (!set_path(actor, "Instigator", repl::Value(p.owner_pawn), e) ||
        !set_path(actor, "ReplicatedServerLastTransformUpdateTimeStamp", repl::Value::f32(static_cast<float>(p.world_time)), e)) {
        error = e;
        return std::nullopt;
    }

    // who the mount is and whose it is
    uint64_t seed = p.seed ? p.seed : w.id;
    repl::GuidValue mount_guid;
    if (p.mount_guid) {
        mount_guid = *p.mount_guid;
    } else {
        mount_guid.a = static_cast<uint32_t>(splitmix(seed) >> 16);
        mount_guid.b = static_cast<uint32_t>(splitmix(seed) >> 16);
        mount_guid.c = static_cast<uint32_t>(splitmix(seed) >> 16);
        mount_guid.d = static_cast<uint32_t>(splitmix(seed) >> 16);
    }
    made.mount_guid = mount_guid;
    repl::GuidValue donor_owner, donor_mount;
    if (repl::Subobject* second = find_sub(a, "UNPCSecondaryInfo")) {
        const char* words[4] = {"CreatureOwnerGuid.A", "CreatureOwnerGuid.B", "CreatureOwnerGuid.C", "CreatureOwnerGuid.D"};
        uint32_t* out[4] = {&donor_owner.a, &donor_owner.b, &donor_owner.c, &donor_owner.d};
        const uint32_t now[4] = {p.owner_guid.a, p.owner_guid.b, p.owner_guid.c, p.owner_guid.d};
        for (int i = 0; i < 4; ++i) {
            *out[i] = static_cast<uint32_t>(util::int_of(second->obj.state.get(*second->obj.schema, words[i])));
            if (!set_path(second->obj, words[i], word_value(now[i]), e)) { error = e; return std::nullopt; }
        }
    }
    if (repl::Subobject* info = find_sub(a, "UNPCInfo")) {
        const char* words[4] = {"CharacterGuid.A", "CharacterGuid.B", "CharacterGuid.C", "CharacterGuid.D"};
        uint32_t* out[4] = {&donor_mount.a, &donor_mount.b, &donor_mount.c, &donor_mount.d};
        const uint32_t now[4] = {mount_guid.a, mount_guid.b, mount_guid.c, mount_guid.d};
        for (int i = 0; i < 4; ++i) {
            *out[i] = static_cast<uint32_t>(util::int_of(info->obj.state.get(*info->obj.schema, words[i])));
            if (!set_path(info->obj, words[i], word_value(now[i]), e)) { error = e; return std::nullopt; }
        }
        if (!set_path(info->obj, "CreatureOwnerName", repl::Value::str(p.owner_name), e)) { error = e; return std::nullopt; }
        if (info->obj.state.get(*info->obj.schema, "ServerId") && !set_path(info->obj, "ServerId", repl::Value::i64(static_cast<int32_t>(p.server_id)), e)) { error = e; return std::nullopt; }
        w.npc.name = p.owner_name;
        if (const repl::Value* lv = info->obj.state.get(*info->obj.schema, "Level")) w.npc.level = static_cast<int>(util::int_of(lv));
    }
    w.npc.level = p.owner_level;
    // the places the capture's two guids stand elsewhere: the alignments, the interaction state
    for (repl::Subobject& s : a.subobjects) {
        replace_guid(s.obj, donor_owner, p.owner_guid);
        replace_guid(s.obj, donor_mount, mount_guid);
    }
    if (repl::Subobject* al = find_sub(a, "UAlignmentComponent")) {
        // The mount's alignments are its OWNER's, never the capture's: a mount inherits what its owner is (the InheritedAlignments of every retail mount are its
        // owner's Alignments: captures 911, 871, 872, 917 hold Player, NonCombatant, the owner's Instigator and PVPSuppressed_Perceiver, plus a NodeCitizen and a
        // GuildMember where the owner had a node and a guild; 898's owner stood in a Lawless zone and was a Combatant, the PvP flag, and so is its mount).  Fix 2 of
        // stage 3: the donors' own sets were kept (their guilds and nodes only zeroed), so a mount of ours carried a PvP flag (898) and a node and a guild (the rest)
        // that its owner had not.  Its own Alignments are the Instigator row with the mount and its owner [R: 911, 898, 871, 872].
        repl::AlignmentContainer inherited;
        if (p.owner_alignments) {
            inherited = *p.owner_alignments;
        } else {
            const repl::GuidValue none;
            inherited.instances = {{bootstrap::kAlignmentKeyA, {none}}, {bootstrap::kAlignmentKeyB, {none}}, {bootstrap::kAlignmentKeyCharacter, {p.owner_guid}},
                                   {bootstrap::kAlignmentKeyC, {none}}};
        }
        repl::AlignmentContainer own;
        own.instances = {{bootstrap::kAlignmentKeyCharacter, {mount_guid, p.owner_guid}}};
        if (!set_path(al->obj, "InheritedAlignments", repl::Value(std::move(inherited)), e) || !set_path(al->obj, "Alignments", repl::Value(std::move(own)), e)) {
            error = e;
            return std::nullopt;
        }
    }
    // the rider's AnimBP: the mount's InteractableComponent names it and the open exports it (see MountBook::load_riders)
    if (repl::Subobject* inter = find_sub(a, "UInteractableComponent")) {
        const repl::ExportChain chain = rider_anim_export(rider_anim_of(row));
        const repl::Value* data = inter->obj.state.get(*inter->obj.schema, "InteractionInputData");
        const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
        if (chain.empty() || !arr) {
            error = row.item_name + ": no rider AnimBP can be named in its interaction data";
            return std::nullopt;
        }
        repl::ArrayValue fixed = *arr;
        size_t named = 0;
        for (repl::ElementState& el : fixed.elements) {
            for (auto& [handle, v] : el.values) {
                const auto* g = v.get_if<FIntrepidNetworkGUID>();
                if (g && g->is_valid()) {   // (the element's only object reference is AttachmentAnimation: the other asset members are soft paths)
                    v = repl::Value(chain.front().guid);
                    ++named;
                }
            }
        }
        if (named == 0) {
            error = row.item_name + ": the donor's interaction data names no AttachmentAnimation";
            return std::nullopt;
        }
        if (!set_path(inter->obj, "InteractionInputData", repl::Value(std::move(fixed)), e)) { error = e; return std::nullopt; }
        w.open_exports.push_back(chain);
    }
    if (repl::Subobject* combat = find_sub(a, "UNPCCombatInfo")) {
        if (!set_path(combat->obj, "bIsDead", repl::Value::boolean(false), e)) { error = e; return std::nullopt; }
    }
// (C | O | A | Cyb)

    // the stats: Health and MaxHealth at the owner's level, StealthLevel the owner's level, no road effect
    made.max_health = max_health(row, p.owner_level);
    if (repl::Subobject* stats = find_sub(a, "UAoCStatsComponent")) {
        if (repl::FastArrayState* arr = stats->obj.arrays.get(*stats->obj.schema, "StatRepInt32EveryoneProxy")) {
            const auto stat_of = [](const repl::FastArrayItem& it) -> const repl::StatInt32Inst* {
                const auto f = it.values.values.find(1);
                return f == it.values.values.end() ? nullptr : f->second.get_if<repl::StatInt32Inst>();
            };
            const double health = std::clamp(p.health ? *p.health : made.max_health, 0.0, made.max_health);
            std::vector<std::pair<int32_t, repl::ElementState>> changes;
            for (const repl::FastArrayItem& it : arr->items()) {
                const repl::StatInt32Inst* t = stat_of(it);
                if (!t) continue;
                float value = std::bit_cast<float>(t->value);
                if (t->record == content::kGuidMaxHealth) value = static_cast<float>(made.max_health);
                else if (t->record == content::kGuidHealth) value = static_cast<float>(health);
                else if (t->record == kStatMoveSpeedMult) value = 1.0f;
                else if (t->record == kStatStealthLevel) value = static_cast<float>(p.owner_level);
                else continue;
                if (std::bit_cast<uint32_t>(value) == t->value) continue;
                repl::StatInt32Inst fresh = *t;
                fresh.value = std::bit_cast<uint32_t>(value);
                repl::ElementState values = it.values;
                values.values[1] = repl::Value(fresh);
                changes.emplace_back(it.id, std::move(values));
            }
            for (auto& [id, values] : changes) {
                if (!arr->change(id, std::move(values))) { error = "a stat of the mount does not take its value"; return std::nullopt; }
            }
        }
        if (repl::FastArrayState* fx = stats->obj.arrays.get(*stats->obj.schema, "CurrentEffects")) {
            std::vector<int32_t> ids;
            for (const repl::FastArrayItem& it : fx->items()) ids.push_back(it.id);
            for (const int32_t id : ids) fx->remove(id);
        }
    }

    // the mount component: the item, summoning
    if (repl::Subobject* mc = find_sub(a, "UMountComponent")) {
        const char* words[4] = {"ItemInstanceGuid.A", "ItemInstanceGuid.B", "ItemInstanceGuid.C", "ItemInstanceGuid.D"};
        const uint32_t now[4] = {p.item_guid.a, p.item_guid.b, p.item_guid.c, p.item_guid.d};
        for (int i = 0; i < 4; ++i) {
            if (!set_path(mc->obj, words[i], word_value(now[i]), e)) { error = e; return std::nullopt; }
        }
        float dissolve = 0.03f + 0.06f * static_cast<float>((splitmix(seed) >> 40) & 0xFFFF) / 65535.0f;   // [R: 0.03 to 0.09 in the captures]
        if (p.dissolve_timer) dissolve = *p.dissolve_timer;
        if (!set_path(mc->obj, "bIsSummoning", repl::Value::boolean(true), e) || !set_path(mc->obj, "DissolveTimer", repl::Value::f32(dissolve), e)) {
            error = e;
            return std::nullopt;
        }
    }
    // the interaction state of the mount is the owner's alone
    world::set_player(a, donor_owner, false);
    world::set_player(a, p.owner_guid, true);

    w.never_dormant = true;
    w.owner = p.player_id;
    return made;
}

}  // namespace aoc::worldd::spawns

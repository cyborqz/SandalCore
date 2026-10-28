// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/npc_looks.cpp
//
//  See npc_looks.h.
//
//  Compiled as part of generic_npc.cpp (which includes it at its end): worldd's source list, server/worldd/CMakeLists.txt, is an input of the walking map's stamp in the
//  extractor, so a new source file would make every PC rebuild the map.  The helpers are named look_* so that they cannot meet generic_npc.cpp's.
// ============================================================================
#include "spawns/npc_looks.h"

#include "spawns/population.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

namespace aoc::worldd::spawns {

namespace {

constexpr const char* kComponent = "UNPCAppearanceComponent";
constexpr const char* kInfo = "UNPCInfo";
constexpr const char* kSecondary = "UNPCSecondaryInfo";
/// The client's idle animations by number (the population data names them): the open carries the index.
constexpr const char* kIdleNames[] = {
    "None", "Sit_Ground", "Sit_Chair", "Sit_Camp", "Cry_Standing", "Cry_Sitting",
    "Cry_Soothing", "CombatReady", "TalkLoop_Talk01", "TalkLoop_Talk02", "TalkLoop_Listen", "Cower",
    "CrossedArms", "Callout01", "Callout02", "Wounded_Seated", "Wounded_Lying", "Kneel_Respectful",
    "Kneel_Resting", "Kneel_Searching", "Kneel_Assist", "AtAttention", "OnGuard", "Lookout",
    "Read_Standing", "Read_Sitting", "TakeInventory", "ChannelEssence", "Tinkering"
};
/// The TypeId of an item appearance record (ItemAppearanceRecordBase): what every AppearanceIDs entry of the captured humanoids carries.
constexpr uint64_t kItemAppearanceType = 7961510888716503441ull;
/// The order the game adds the slots to AppearanceIDs (head, chest, wrists, legs, shoulders, back, waist, hands, feet, primary and secondary held item).
constexpr int kSlotOrder[] = {0, 2, 3, 5, 7, 8, 9, 4, 6, 15, 16};

uint64_t look_mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// A small generator over a seed: every draw of one NPC comes from its point's seed, so a look does not change from one start to the next.
struct LookRng {
    uint64_t state;
    explicit LookRng(uint64_t seed) : state(seed) {}
    uint64_t next() { return look_mix(state += 0x9E3779B97F4A7C15ull); }
    double   unit() { return static_cast<double>(next() >> 11) / static_cast<double>(1ull << 53); }
    size_t   index(size_t n) { return n == 0 ? 0 : static_cast<size_t>(next() % n); }
};

std::vector<std::string> look_split(const std::string& line, char sep) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t at = line.find(sep, from);
        if (at == std::string::npos) {
            out.push_back(line.substr(from));
            return out;
        }
        out.push_back(line.substr(from, at - from));
        from = at + 1;
    }
}

bool look_read_lines(const std::string& path, std::vector<std::string>& lines, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = path + " cannot be opened: run scripts/extract_game_data.ps1";
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return true;
}

bool look_to_i64(const std::string& s, int64_t& v) {
    if (s.empty()) return false;
    const char* b = s.data();
    const char* e = b + s.size();
    const auto r = std::from_chars(b, e, v);
    return r.ec == std::errc() && r.ptr == e;
}

std::vector<int64_t> look_int_list(const std::string& s) {
    std::vector<int64_t> out;
    if (s.empty()) return out;
    for (const std::string& p : look_split(s, ',')) {
        int64_t v = 0;
        if (look_to_i64(p, v)) out.push_back(v);
    }
    return out;
}

int look_race_number(const std::string& name) {
    static const char* const kRaces[] = {"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua", "Tulnar"};
    for (int i = 0; i < 9; ++i) {
        if (name == kRaces[i]) return i;
    }
    return 0;
}

bool look_blank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

std::string look_leaf(const std::string& archetype_path) {
    std::string leaf = archetype_path.substr(archetype_path.rfind('.') == std::string::npos ? 0 : archetype_path.rfind('.') + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    return leaf;
}

int64_t look_as_i64(const repl::Value* v) {
    if (!v) return 0;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

repl::Subobject* look_find_sub(repl::Actor& a, const char* cls) {
    for (repl::Subobject& s : a.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

}  // namespace

// ---- the curves ---------------------------------------------------------------------------------------------------

LookCurve LookCurve::parse(const std::string& text) {
    LookCurve c;
    if (text.empty()) return c;
    for (const std::string& k : look_split(text, ',')) {
        const std::vector<std::string> f = look_split(k, ':');
        if (f.size() != 3) continue;
        Key key;
        key.time = std::strtod(f[0].c_str(), nullptr);
        key.value = std::strtod(f[1].c_str(), nullptr);
        key.mode = f[2].empty() ? 'K' : f[2][0];
        c.keys_.push_back(key);
    }
    std::stable_sort(c.keys_.begin(), c.keys_.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
    return c;
}

double LookCurve::eval(double t) const {
    if (keys_.empty()) return 0;
    if (t <= keys_.front().time) return keys_.front().value;
    if (t >= keys_.back().time) return keys_.back().value;
    for (size_t i = 0; i + 1 < keys_.size(); ++i) {
        const Key& a = keys_[i];
        const Key& b = keys_[i + 1];
        if (t >= b.time) continue;
        const double dt = b.time - a.time;
        if (dt <= 0) return a.value;
        const double x = (t - a.time) / dt;
        switch (a.mode) {
            case 'L': return a.value + (b.value - a.value) * x;
            case 'C': return a.value + (b.value - a.value) * (x * x * (3.0 - 2.0 * x));   // a cubic key pair with the data's zero tangents
            default: return a.value;
        }
    }
    return keys_.back().value;
}

double LookCurve::min_value() const {
    double m = keys_.empty() ? 0 : keys_.front().value;
    for (const Key& k : keys_) m = std::min(m, k.value);
    return m;
}

int weighted_index(size_t count, const LookCurve& curve, double u) {
    if (count == 0) return -1;
    if (count == 1) return 0;
    const double shift = !curve.empty() && curve.min_value() < 0 ? -curve.min_value() : 0.0;
    std::vector<double> running(count);
    double sum = 0;
    for (size_t i = 0; i < count; ++i) {
        double w = 1.0;
        if (!curve.empty()) {
            const double t = curve.first_time() + (curve.last_time() - curve.first_time()) * (static_cast<double>(i) / static_cast<double>(count - 1));
            w = curve.eval(t) + shift;
        }
        sum += w;
        running[i] = sum;
    }
    const double r = u * sum;
    for (size_t i = 0; i < count; ++i) {
        if (r <= running[i]) return static_cast<int>(i);
    }
    return static_cast<int>(count - 1);
}

// ---- loading ------------------------------------------------------------------------------------------------------

std::unique_ptr<NpcLooks> NpcLooks::load(const std::string& dir, const npc::MotionTable* motion, const content::CombatRows* combat, const CapturedData* captured, std::string& error) {
    auto out = std::unique_ptr<NpcLooks>(new NpcLooks());
    out->motion_ = motion;
    const std::string base = dir.empty() || dir.back() == '/' || dir.back() == '\\' ? dir : dir + "/";
    std::vector<std::string> lines;
    if (!look_read_lines(base + "npc_looks.tsv", lines, error)) return nullptr;
    for (size_t i = 1; i < lines.size(); ++i) {   // (the first line is the header)
        const std::vector<std::string> f = look_split(lines[i], '\t');
        if (f.size() < 5) continue;
        BlueprintLook b;
        if (!look_to_i64(f[1], b.appearance)) continue;
        b.name = f[2];
        b.race = look_race_number(f[3]);
        b.gender = f[4] == "Female" ? 1 : 0;
        if (f.size() > 5) b.info_name = f[5];
        out->blueprint_[f[0]] = b;
    }
    lines.clear();
    if (!look_read_lines(base + "npc_appearances.tsv", lines, error)) return nullptr;
    for (const std::string& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = look_split(line, '\t');
        int64_t guid = 0;
        if (f.size() < 2 || !look_to_i64(f[1], guid)) continue;
        if (f[0] == "A" && f.size() >= 10) {
            Appearance a;
            a.name = f[2];
            look_to_i64(f[3], a.shared_info);
            a.randomize_custom = f[4] == "1";
            a.randomize_race_gender = f[5] == "1";
            a.hair_natural = f[6] == "1";
            look_to_i64(f[7], a.seed);
            a.curve = LookCurve::parse(f[8]);
            a.outfits = look_int_list(f[9]);
            out->appearance_[guid] = std::move(a);
        } else if (f[0] == "O" && f.size() >= 5) {
            Outfit o;
            o.name = f[2];
            o.single_curve = f[3] == "1";
            o.curve = LookCurve::parse(f[4]);
            out->outfit_[guid] = std::move(o);
        } else if (f[0] == "S" && f.size() >= 4) {
            int64_t slot = 0;
            if (look_to_i64(f[2], slot)) out->outfit_[guid].slot_curves[static_cast<int>(slot)] = LookCurve::parse(f[3]);
        } else if (f[0] == "I" && f.size() >= 4) {
            int64_t slot = 0;
            if (look_to_i64(f[2], slot)) out->outfit_[guid].items[static_cast<int>(slot)] = look_int_list(f[3]);
        } else if (f[0] == "R" && f.size() >= 4) {
            std::vector<std::pair<int, int>> pairs;
            if (!f[3].empty()) {
                for (const std::string& p : look_split(f[3], ',')) {
                    const std::vector<std::string> rg = look_split(p, ':');
                    if (rg.size() == 2) pairs.emplace_back(std::atoi(rg[0].c_str()), std::atoi(rg[1].c_str()));
                }
            }
            out->races_[guid] = std::move(pairs);
        }
    }
    lines.clear();
    if (!look_read_lines(base + "npc_names.tsv", lines, error)) return nullptr;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::vector<std::string> f = look_split(lines[i], '\t');
        int64_t source = 0, member = 0;
        if (f.size() < 4 || !look_to_i64(f[1], source) || !look_to_i64(f[2], member)) continue;
        if (f[0] == "instance") out->instance_names_[source].push_back(f[3]);
        else if (f[0] == "asset") out->asset_names_[source].push_back(f[3]);
        else if (f[0] == "set") out->set_names_[{source, member}].push_back(f[3]);
    }
    // the alignments that make a creature the players' enemy: a relationship with the player's alignment whose permission is Hostile, either way round
    if (combat) {
        const int64_t player = content::kAlignmentPlayer;
        const auto hostile_between = [&](int64_t x, int64_t y) {
            const auto ax = combat->relation.find(x);
            if (ax == combat->relation.end()) return false;
            const auto r = ax->second.find(y);
            if (r == ax->second.end()) return false;
            const auto p = combat->permissions.find(r->second);
            return p != combat->permissions.end() && p->second == content::kPermissionHostile;
        };
        std::set<int64_t> all;
        for (const auto& [a, others] : combat->relation) {
            all.insert(a);
            for (const auto& [o, r] : others) all.insert(o);
        }
        for (const int64_t a : all) {
            if (hostile_between(a, player) || hostile_between(player, a)) out->hostile_alignments_.insert(a);
        }
    }
    // the customization pool: what the retail captures hold for each shared info, race and gender
    if (captured) {
        for (const OpenTemplate& t : captured->opens()) {
            if (t.category != "npc") continue;
            const int ci = t.find(kComponent);
            if (ci <= 0) continue;
            const ParsedBlock& b = t.blocks[static_cast<size_t>(ci)];
            if (!b.schema) continue;
            const repl::Value* cv = b.state.state.get(*b.schema, "CharacterCustomization");
            const auto* cc = cv ? cv->get_if<repl::CharacterCustomization>() : nullptr;
            const int64_t shared = look_as_i64(b.state.state.get(*b.schema, "SharedAppearanceInfoId.Guid"));
            if (!cc || !cc->race_gender || shared == 0) continue;
            out->pool_[{shared, cc->race_gender->race, cc->race_gender->gender}].push_back(*cc);
        }
    }
    return out;
}

// ---- the look -----------------------------------------------------------------------------------------------------

const NpcLooks::Appearance* NpcLooks::appearance(int64_t guid) const {
    const auto it = appearance_.find(guid);
    return it == appearance_.end() ? nullptr : &it->second;
}

const NpcLooks::Outfit* NpcLooks::outfit(int64_t guid) const {
    const auto it = outfit_.find(guid);
    return it == outfit_.end() ? nullptr : &it->second;
}

const NpcLooks::BlueprintLook* NpcLooks::blueprint(const std::string& archetype_path) const {
    const auto it = blueprint_.find(archetype_path);
    return it == blueprint_.end() ? nullptr : &it->second;
}

bool NpcLooks::hostile(const std::vector<int64_t>& alignments) const {
    for (const int64_t a : alignments) {
        if (hostile_alignments_.count(a)) return true;
    }
    return false;
}

int64_t NpcLooks::appearance_of(const content::NpcAssetRow& asset) const {
    if (asset.appearance != 0 && appearance_.count(asset.appearance)) return asset.appearance;
    if (const BlueprintLook* b = blueprint(asset.archetype_path); b && appearance_.count(b->appearance)) return b->appearance;
    return 0;
}

bool NpcLooks::is_humanoid(const content::NpcAssetRow& asset) const {
    if (!motion_ || asset.archetype_path.empty()) return false;
    const auto m = motion_->find(asset.archetype_path);
    return m && m->appearance_dependent();
}

bool NpcLooks::can_dress(const content::NpcAssetRow& asset) const {
    if (!motion_ || asset.archetype_path.empty()) return false;
    const auto m = motion_->find(asset.archetype_path);
    if (!m || !m->appearance_dependent() || hostile(m->alignments)) return false;
    const Appearance* a = appearance(appearance_of(asset));
    if (!a || a->shared_info == 0) return false;
    for (const int64_t og : a->outfits) {
        const Outfit* o = outfit(og);
        if (!o) continue;
        for (const auto& [slot, list] : o->items) {
            if (!list.empty()) return true;
        }
    }
    return false;
}

bool NpcLooks::talks(const content::NpcAssetRow& asset, const content::NpcInstanceRow* i) {
    if (asset.dialogue_root != 0 || asset.interact_component != 0 || asset.interaction_def != 0) return true;
    return i && (i->vendor != 0 || i->interact_component != 0 || i->interact_response != 0 || i->dialogue != 0 || i->dialogue_root != 0);
}

std::vector<repl::AppearanceId> NpcLooks::items_of(int64_t appearance_guid, uint64_t seed) const {
    std::vector<repl::AppearanceId> out;
    const Appearance* a = appearance(appearance_guid);
    if (!a) return out;
    LookRng rng(look_mix(seed ^ 0x0071F175ull));
    const int pick = weighted_index(a->outfits.size(), a->curve, rng.unit());
    if (pick < 0) return out;
    const Outfit* o = outfit(a->outfits[static_cast<size_t>(pick)]);
    if (!o) return out;
    for (const int slot : kSlotOrder) {
        const auto it = o->items.find(slot);
        if (it == o->items.end() || it->second.empty()) continue;
        const LookCurve* curve = &o->curve;
        if (!o->single_curve) {
            if (const auto sc = o->slot_curves.find(slot); sc != o->slot_curves.end()) curve = &sc->second;
        }
        const int i = weighted_index(it->second.size(), *curve, rng.unit());
        if (i < 0 || it->second[static_cast<size_t>(i)] == 0) continue;
        repl::AppearanceId id;
        id.record_guid = static_cast<uint64_t>(it->second[static_cast<size_t>(i)]);
        id.type_id = kItemAppearanceType;
        id.enchantment_level = 0;
        id.item_equip_slot = static_cast<uint8_t>(slot);
        out.push_back(id);
    }
    return out;
}

// ---- the name -----------------------------------------------------------------------------------------------------

std::optional<std::string> NpcLooks::name_of(const content::NpcInstanceRow* instance, const content::NpcAssetRow& asset, int64_t asset_set, uint64_t seed) const {
    const std::vector<std::string>* list = nullptr;
    if (instance) {
        if (const auto it = instance_names_.find(instance->guid); it != instance_names_.end() && !it->second.empty()) list = &it->second;
    }
    if (!list && asset_set != 0) {
        if (const auto it = set_names_.find({asset_set, asset.guid}); it != set_names_.end() && !it->second.empty()) list = &it->second;
    }
    if (!list) {
        if (const auto it = asset_names_.find(asset.guid); it != asset_names_.end() && !it->second.empty()) list = &it->second;
    }
    if (!list) return std::nullopt;
    LookRng rng(look_mix(seed ^ 0x4E414D45ull));
    const std::string& text = (*list)[rng.index(list->size())];
    if (look_blank(text)) return std::nullopt;   // an empty entry is not a name: the Blueprint's stays
    return text;
}

// ---- dressing -----------------------------------------------------------------------------------------------------

bool NpcLooks::dress(world::WorldActor& w, const DataSpawn& s, std::string& error) const {
    if (!s.asset || !is_humanoid(*s.asset)) return true;   // a creature: nothing to dress
    repl::Subobject* look = look_find_sub(w.actor, kComponent);
    if (!look || !look->obj.schema) {
        ++counts_.no_component;
        error = "the open of " + look_leaf(s.asset->archetype_path) + " has no appearance component";
        return false;
    }
    const int64_t guid = appearance_of(*s.asset);
    const Appearance* app = appearance(guid);
    const BlueprintLook* bp = blueprint(s.asset->archetype_path);
    const uint64_t seed = look_mix(s.seed ^ 0x100C5ull);
    if (!app || app->shared_info == 0) {
        // no record to dress by (the game's own records with no shared info or no component class make no appearance component either): the recorded look stays, the name is the data's
        ++counts_.no_record;
        return apply_name(w, s, bp, seed, error);
    }
    LookRng rng(seed);
    const schema::ClassSchema& cs = *look->obj.schema;
    // the clothes
    const std::vector<repl::AppearanceId> items = items_of(guid, seed);
    repl::ArrayValue ids;
    for (const repl::AppearanceId& id : items) {
        repl::ElementState e;
        e.values[1] = repl::Value(id);
        ids.elements.push_back(std::move(e));
    }
    if (!look->obj.state.set(cs, "AppearanceIDs", repl::Value(std::move(ids)))) {
        error = "AppearanceIDs refused";
        return false;
    }
    if (!look->obj.state.set(cs, "SharedAppearanceInfoId.Guid", repl::Value::i64(app->shared_info))) {
        error = "SharedAppearanceInfoId refused";
        return false;
    }
    if (cs.command("NPCAppearanceId.Guid") && !look->obj.state.set(cs, "NPCAppearanceId.Guid", repl::Value::i64(guid))) {
        error = "NPCAppearanceId refused";
        return false;
    }
    // the person: race and gender, then a recorded customization of the same kind
    int race = bp ? bp->race : 0, gender = bp ? bp->gender : 0;
    if (app->randomize_race_gender) {
        if (const auto r = races_.find(app->shared_info); r != races_.end() && !r->second.empty()) {
            const auto& pick = r->second[rng.index(r->second.size())];
            race = pick.first;
            gender = pick.second;
        }
    }
    repl::CharacterCustomization cc;
    if (const auto p = pool_.find({app->shared_info, race, gender}); p != pool_.end() && !p->second.empty()) {
        cc = p->second[rng.index(p->second.size())];
        ++counts_.from_pool;
    } else {
        ++counts_.plain_customization;
    }
    cc.preset_guid.reset();
    cc.race_gender = repl::CharacterCustomization::RaceGender{static_cast<uint8_t>(race), static_cast<uint8_t>(gender)};
    cc.random_seed = static_cast<uint32_t>((rng.next() >> 33) | 1u);
    if (!look->obj.state.set(cs, "CharacterCustomization", repl::Value(std::move(cc)))) {
        error = "CharacterCustomization refused";
        return false;
    }
    if (!apply_name(w, s, bp, seed, error)) return false;
    ++counts_.dressed;
    return true;
}

bool NpcLooks::apply_name(world::WorldActor& w, const DataSpawn& s, const BlueprintLook* bp, uint64_t seed, std::string& error) const {
    // the name: the data's, else none at all (the client shows the Blueprint's own, as for the game's own villagers)
    if (repl::Subobject* info = look_find_sub(w.actor, kInfo); info && info->obj.schema) {
        const std::optional<std::string> name = name_of(s.instance, *s.asset, s.asset_set, seed);
        const schema::RepCommand* cmd = info->obj.schema->command("CharacterName");
        if (name) {
            if (cmd && !info->obj.state.set(*info->obj.schema, "CharacterName", repl::Value::str(*name))) {
                error = "CharacterName refused";
                return false;
            }
            w.npc.name = *name;
            ++counts_.named;
        } else if (bp && !bp->name.empty() && bp->name != bp->info_name) {
            // the Blueprint's own name: the game's server copies it into the NPC's info, and the open carries it when it differs from the info component's own default (the Black Market
            // vendor's open says "Ophelia Blackwater"; Sosha Terim's Blueprint names her in the component too, so her open says nothing)
            if (cmd && !info->obj.state.set(*info->obj.schema, "CharacterName", repl::Value::str(bp->name))) {
                error = "CharacterName refused";
                return false;
            }
            w.npc.name = bp->name;
            ++counts_.blueprint_named;
        } else {
            if (cmd) info->obj.state.erase(cmd->handle);   // nothing to say beyond what the client's component already holds (or a villager: no name anywhere)
            w.npc.name = bp && !bp->name.empty() ? bp->name : look_leaf(s.asset->archetype_path);
            ++counts_.unnamed;
        }
    }
    return true;
}

int idle_animation_number(const std::string& name) {
    for (size_t i = 0; i < std::size(kIdleNames); ++i) {
        if (name == kIdleNames[i]) return static_cast<int>(i);
    }
    return -1;
}

bool apply_idle_animation(world::WorldActor& w, const content::NpcInstanceRow* instance, std::string& error) {
    if (!instance) return true;
    repl::Subobject* second = look_find_sub(w.actor, kSecondary);
    if (!second || !second->obj.schema) return true;
    const schema::RepCommand* cmd = second->obj.schema->command("IdleAnimation");
    if (!cmd) return true;
    const int number = idle_animation_number(instance->idle_anim.empty() ? std::string("None") : instance->idle_anim);
    if (number < 0) {
        error = "the idle animation " + instance->idle_anim + " is not one the client knows";
        return false;
    }
    if (number == 0) {
        second->obj.state.erase(cmd->handle);   // none: the open leaves the field out
        return true;
    }
    if (!second->obj.state.set(*second->obj.schema, "IdleAnimation", repl::Value::u64(static_cast<uint64_t>(number)))) {
        error = "IdleAnimation refused";
        return false;
    }
    return true;
}

std::string NpcLooks::describe() const {
    std::ostringstream o;
    size_t pooled = 0;
    for (const auto& [k, v] : pool_) pooled += v.size();
    o << blueprint_.size() << " Blueprints with a look, " << appearance_.size() << " appearance records, " << outfit_.size() << " outfits, names of " << instance_names_.size() << " instances, "
      << set_names_.size() << " asset set entries and " << asset_names_.size() << " assets, " << hostile_alignments_.size() << " alignments hostile to players, " << pooled
      << " recorded customizations in " << pool_.size() << " kinds";
    return o.str();
}

}  // namespace aoc::worldd::spawns

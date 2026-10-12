// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/character_setup.cpp
// ============================================================================
#include "bootstrap/character_setup.h"

#include "bootstrap/captured_abilities.h"
#include "bootstrap/captured_setup.h"
#include "protocol/repl/codecs.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace aoc::worldd::bootstrap {

namespace {

using json = nlohmann::ordered_json;

// ---- race and gender ------------------------------------------------------------------------------------------------

constexpr const char* kRaceNames[] = {"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua", "Tulnar"};
constexpr const char* kGenderNames[] = {"Male", "Female"};
// ECharacterClass, indexed by the id (0 is no class)
constexpr const char* kClassNames[] = {nullptr, "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"};

// ---- the body section enum (FCharacterCustomizationBodySections, the exe's name table at 0x14b74ff50) ------------

constexpr const char* kBodySections[kBodySectionCount] = {
    "None", "LowerArms", "UpperArms", "Clavicle", "Thigh", "Calf", "Neck", "LowerAbdomen", "Abdomen", "Chest",
    "Hips", "Size", "MouthGroup", "NoseGroup", "EyeGroup", "BrowGroup", "EarGroup", "Jaw", "EyeSocketL", "EyeSocketR",
    "EyeInnerR", "EyeInnerL", "EyeOuterL", "EyeOuterR", "SmilerL", "SmilerR", "Forehead", "Cheeks", "RacialGroup", "cCount",
};

std::string lower_first(std::string s) {
    if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z') s[0] = static_cast<char>(s[0] - 'A' + 'a');
    return s;
}

std::string upper_first(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

// ---- FName ----------------------------------------------------------------------------------------------------------

/// FName(const char*) as UE 5 builds it: a trailing "_<digits>" (1 to 10 digits, no leading zero unless the number is
/// "0", below INT32_MAX, and something before the underscore) is the name's number, stored as digits + 1; the plain name
/// is what precedes the underscore.  UPackageMap::SerializeName sends the plain string and that number: the capture's
/// morph "Racial_2" is "Racial" and 3 (hex 52616369616c00 03000000 in pkt#78).
repl::NameValue name_from_string(const std::string& s) {
    size_t digits = 0;
    while (digits < s.size() && s[s.size() - 1 - digits] >= '0' && s[s.size() - 1 - digits] <= '9') ++digits;
    if (digits > 0 && digits < s.size() && s[s.size() - 1 - digits] == '_' && digits <= 10) {
        const char* first = s.data() + s.size() - digits;
        if (first[0] != '0' || digits == 1) {
            int64_t n = 0;
            std::from_chars(first, s.data() + s.size(), n);
            if (n < std::numeric_limits<int32_t>::max()) {
                return repl::NameValue::plain(s.substr(0, s.size() - digits - 1), static_cast<int32_t>(n + 1));
            }
        }
    }
    return repl::NameValue::plain(s, 0);
}
// C.O.A.C-Y-B

/// The string an FName with this plain text and number displays as; nullopt for a hardcoded name (no text).
std::optional<std::string> name_to_string(const repl::NameValue& n) {
    if (n.hardcoded) return std::nullopt;
    if (n.number == 0) return n.text;
    return n.text + "_" + std::to_string(static_cast<int64_t>(n.number) - 1);
}

// ---- reading the JSON ---------------------------------------------------------------------------------------------

enum class Kind { Float, Int32, Id };

struct FieldSpec {
    repl::CustomizationField field;
    const char*              key;
    Kind                     kind;
};

using F = repl::CustomizationField;
// The 30 scalar members in wire order (codecs.cpp kCustomizationBits: 32-bit ones are floats but SkinSet, the 64-bit ones ids).
constexpr FieldSpec kFields[repl::kCustomizationFieldCount] = {
    {F::SkinColorHue, "skinColorHue", Kind::Float},
    {F::SkinColorPigmentation, "skinColorPigmentation", Kind::Float},
    {F::NormalDetailStrength01, "normalDetailStrength01", Kind::Float},
    {F::NormalDetailStrength02, "normalDetailStrength02", Kind::Float},
    {F::SkinSet, "skinSet", Kind::Int32},
    {F::EyeColor, "eyeColor", Kind::Id},
    {F::EyeShape, "eyeShape", Kind::Id},
    {F::ScleraShape, "scleraShape", Kind::Id},
    {F::Eyebrows, "eyebrows", Kind::Id},
    {F::HeadHair, "headHair", Kind::Id},
    {F::HeadHairRootColor, "headHairRootColor", Kind::Id},
    {F::HeadHairTipColor, "headHairTipColor", Kind::Id},
    {F::HeadHairLength, "headHairLength", Kind::Float},
    {F::HeadHairContrast, "headHairContrast", Kind::Float},
    {F::HeadHairGradient, "headHairGradient", Kind::Float},
    {F::FacialHairLip, "facialHairLip", Kind::Id},
    {F::FacialHairChin, "facialHairChin", Kind::Id},
    {F::FacialHairCheek, "facialHairCheek", Kind::Id},
    {F::FacialHairLipLength, "facialHairLipLength", Kind::Float},
    {F::FacialHairChinLength, "facialHairChinLength", Kind::Float},
    {F::FacialHairCheekLength, "facialHairCheekLength", Kind::Float},
    {F::EyelashLength, "eyelashLength", Kind::Float},
    {F::FacialHairRootColor, "facialHairRootColor", Kind::Id},
    {F::FacialHairTipColor, "facialHairTipColor", Kind::Id},
    {F::FacialHairContrast, "facialHairContrast", Kind::Float},
    {F::FacialHairGradient, "facialHairGradient", Kind::Float},
    {F::RacialHorns, "racialHorns", Kind::Id},
    {F::RacialHornsLength, "racialHornsLength", Kind::Float},
    {F::NailColor, "nailColor", Kind::Id},
    {F::NailOpacity, "nailOpacity", Kind::Float},
};
static_assert(static_cast<size_t>(F::NailOpacity) + 1 == repl::kCustomizationFieldCount, "kFields covers every CustomizationField");

/// A reader of one JSON value that fails with a message naming the member.
struct Reader {
    std::string error;

    bool fail(const std::string& path, const char* what) {
        error = "appearance: '" + path + "' " + what;
        return false;
    }

    /// A float member: a JSON number, taken as the float32 the client holds (its JSON is that float widened to a double).
    bool read_float(const json& v, const std::string& path, uint32_t& bits) {
        if (!v.is_number()) return fail(path, "is not a number");
        const double d = v.get<double>();
        const float f = static_cast<float>(d);
        if (!std::isfinite(f)) return fail(path, "is not a finite float");
        bits = std::bit_cast<uint32_t>(f);
        return true;
    }

    bool read_int32(const json& v, const std::string& path, uint32_t& bits) {
        if (!(v.is_number_integer() || v.is_number_unsigned())) return fail(path, "is not an integer");
        const int64_t n = v.is_number_unsigned() ? (v.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                                                        ? std::numeric_limits<int64_t>::max()
                                                        : static_cast<int64_t>(v.get<uint64_t>()))
                                                 : v.get<int64_t>();
        if (n < std::numeric_limits<int32_t>::min() || n > std::numeric_limits<int32_t>::max()) return fail(path, "does not fit 32 bits");
        bits = static_cast<uint32_t>(static_cast<int32_t>(n));
        return true;
    }

    bool read_u8(const json& v, const std::string& path, uint8_t& out) {
        if (!(v.is_number_integer() || v.is_number_unsigned())) return fail(path, "is not an integer");
        if (v.is_number_integer() && !v.is_number_unsigned() && v.get<int64_t>() < 0) return fail(path, "does not fit a byte");
        if (v.get<uint64_t>() > 255) return fail(path, "does not fit a byte");
        out = static_cast<uint8_t>(v.get<uint64_t>());
        return true;
    }

    /// A 64-bit id: UE writes an int64 as a decimal string; a JSON integer is accepted too. The wire carries the bits.
    bool read_id(const json& v, const std::string& path, uint64_t& bits) {
        if (v.is_number_integer() || v.is_number_unsigned()) {
            bits = v.is_number_unsigned() ? v.get<uint64_t>() : static_cast<uint64_t>(v.get<int64_t>());
            return true;
        }
        if (!v.is_string()) return fail(path, "is not an id (a decimal string)");
        const std::string& s = v.get_ref<const std::string&>();
        if (s.empty()) return fail(path, "is an empty id");
        const char* end = s.data() + s.size();
        int64_t signed_value = 0;
        auto r = std::from_chars(s.data(), end, signed_value);
        if (r.ec == std::errc() && r.ptr == end) {
            bits = static_cast<uint64_t>(signed_value);
            return true;
        }
        uint64_t unsigned_value = 0;
        r = std::from_chars(s.data(), end, unsigned_value);
        if (r.ec == std::errc() && r.ptr == end) {
            bits = unsigned_value;
            return true;
        }
        return fail(path, "is not a 64-bit decimal id");
    }

    bool read_bool(const json& v, const std::string& path, bool& out) {
        if (!v.is_boolean()) return fail(path, "is not true or false");
        out = v.get<bool>();
        return true;
    }

    bool read_double(const json& v, const std::string& path, double& out) {
        if (!v.is_number()) return fail(path, "is not a number");
        out = v.get<double>();
        if (!std::isfinite(out)) return fail(path, "is not finite");
        return true;
    }

    /// The member `key` of an object that has to be there.
    const json* required(const json& obj, const char* key, const std::string& path) {
        const auto it = obj.find(key);
        if (it == obj.end()) {
            fail(path + "." + key, "is missing");
            return nullptr;
        }
        return &*it;
    }

    bool read_decal(const json& e, const std::string& path, repl::CustomizationDecal& d) {
        if (!e.is_object()) return fail(path, "is not an object");
        const auto id = [&](const char* key, uint64_t& out) {
            const json* v = required(e, key, path);
            return v && read_id(*v, path + "." + key, out);
        };
        const auto flt = [&](const char* key, uint32_t& out) {
            const json* v = required(e, key, path);
            return v && read_float(*v, path + "." + key, out);
        };
        const json* slot = required(e, "slotIndex", path);
        return id("guid", d.guid) && flt("strength", d.strength) && flt("scaleU", d.scale_u) && flt("scaleV", d.scale_v) &&
               slot && read_u8(*slot, path + ".slotIndex", d.slot_index) && id("mergeSectionMaterialBase", d.merge_section_material_base) &&
               flt("offsetU", d.offset_u) && flt("offsetV", d.offset_v) && flt("angle", d.angle) && id("color", d.color) &&
               flt("discoloration", d.discoloration);
    }
// CoA x C-Y-B

    bool read_blend_group(const json& e, const std::string& path, repl::CustomizationBlendGroup& g) {
        if (!e.is_object()) return fail(path, "is not an object");
        const json* a = required(e, "decalAppearanceGuid", path);
        const json* c = required(e, "colorGuid", path);
        const json* i = required(e, "intensity", path);
        return a && c && i && read_id(*a, path + ".decalAppearanceGuid", g.decal_appearance_guid) &&
               read_id(*c, path + ".colorGuid", g.color_guid) && read_float(*i, path + ".intensity", g.intensity);
    }
};

bool is_known_key(const std::string& key) {
    static const char* const kOthers[] = {"presetGuid", "randomSeed", "decalData", "decalBlendGroups", "bIsHelmetVisible",
                                          "bIsCapeVisible", "gender", "race", "class", "faceMorphWeightMaps", "sectionsValues"};
    for (const char* k : kOthers)
        if (key == k) return true;
    for (const FieldSpec& f : kFields)
        if (key == f.key) return true;
    return false;
}

// ---- the hex id ---------------------------------------------------------------------------------------------------------

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

// ---- race and gender --------------------------------------------------------------------------------------------------

std::optional<ClientRace> client_race_from_id(int32_t race_id) {
    // ECharacterRace is the client's own enum, and what the character-select protocol and the pawn carry (plan B1), so the
    // id is the race: Kaelar 0 to Nikua 7.  Tulnar (8) has no player appearance in the design data.
    if (race_id < static_cast<int32_t>(ClientRace::Kaelar) || race_id > static_cast<int32_t>(ClientRace::Nikua)) return std::nullopt;
    return static_cast<ClientRace>(race_id);
}

std::optional<ClientGender> client_gender_from_id(int32_t gender_id) {
    if (gender_id < static_cast<int32_t>(ClientGender::Male) || gender_id > static_cast<int32_t>(ClientGender::Female)) return std::nullopt;
    return static_cast<ClientGender>(gender_id);
}

const char* client_class_name(int32_t class_id) {
    return class_id >= 1 && class_id < static_cast<int32_t>(std::size(kClassNames)) ? kClassNames[class_id] : nullptr;
}

const char* client_race_name(ClientRace race) {
    const size_t i = static_cast<size_t>(race);
    return i < std::size(kRaceNames) ? kRaceNames[i] : nullptr;
}

const char* client_gender_name(ClientGender gender) {
    const size_t i = static_cast<size_t>(gender);
    return i < std::size(kGenderNames) ? kGenderNames[i] : nullptr;
}

std::optional<ClientRace> client_race_from_name(std::string_view name) {
    for (size_t i = 0; i < std::size(kRaceNames); ++i)
        if (name == kRaceNames[i]) return static_cast<ClientRace>(i);
    return std::nullopt;
}

std::optional<ClientGender> client_gender_from_name(std::string_view name) {
    for (size_t i = 0; i < std::size(kGenderNames); ++i)
        if (name == kGenderNames[i]) return static_cast<ClientGender>(i);
    return std::nullopt;
}

const char* body_section_name(uint8_t value) { return value < kBodySectionCount ? kBodySections[value] : nullptr; }

// ---- customization ------------------------------------------------------------------------------------------------------

CustomizationFromJson customization_from_json(std::string_view appearance_json, ClientRace race, ClientGender gender) {
    CustomizationFromJson out;
    if (appearance_json.size() > kMaxAppearanceBytes) {
        out.error = "appearance: too large (" + std::to_string(appearance_json.size()) + " bytes; the most is " +
                    std::to_string(kMaxAppearanceBytes) + ")";
        return out;
    }
    const json j = json::parse(appearance_json.begin(), appearance_json.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        out.error = "appearance: the character's appearance is not valid JSON";
        return out;
    }
    if (!j.is_object()) {
        out.error = "appearance: the character's appearance is not a JSON object";
        return out;
    }

    Reader rd;
    repl::CharacterCustomization c;

    if (const auto it = j.find("presetGuid"); it != j.end()) {
        uint64_t preset = 0;
        if (!rd.read_id(*it, "presetGuid", preset)) {
            out.error = rd.error;
            return out;
        }
        if (preset != 0) {
            out.error = "appearance: presetGuid " + std::to_string(preset) +
                        " is a preset: whether the client's preset table has it, which decides if Race and Gender are on the wire, is not known";
            return out;
        }
    }
    if (const auto it = j.find("randomSeed"); it != j.end()) {
        uint32_t seed = 0;
        if (!rd.read_int32(*it, "randomSeed", seed)) {
            out.error = rd.error;
            return out;
        }
        if (seed != 0) c.random_seed = seed;      // sent when non-zero
    }

    // the client has no preset for presetGuid 0, so Race and Gender are on the wire (capture: Race 0, Gender 0)
    c.race_gender = repl::CharacterCustomization::RaceGender{static_cast<uint8_t>(race), static_cast<uint8_t>(gender)};
// == Cyb | C | O | A ==

    for (const FieldSpec& f : kFields) {
        const auto it = j.find(f.key);
        if (it == j.end()) continue;
        uint32_t bits32 = 0;
        uint64_t bits64 = 0;
        bool ok = false;
        switch (f.kind) {
        case Kind::Float: ok = rd.read_float(*it, f.key, bits32); bits64 = bits32; break;
        case Kind::Int32: ok = rd.read_int32(*it, f.key, bits32); bits64 = bits32; break;
        case Kind::Id: ok = rd.read_id(*it, f.key, bits64); break;
        }
        if (!ok) {
            out.error = rd.error;
            return out;
        }
        c.deltas[f.field] = bits64;
    }

    if (const auto it = j.find("decalData"); it != j.end()) {
        if (!it->is_array()) {
            out.error = "appearance: 'decalData' is not an array";
            return out;
        }
        std::vector<std::optional<repl::CustomizationDecal>> v;
        for (size_t i = 0; i < it->size(); ++i) {
            const json& e = (*it)[i];
            if (e.is_null()) {
                v.push_back(std::nullopt);
                continue;
            }
            repl::CustomizationDecal d;
            if (!rd.read_decal(e, "decalData[" + std::to_string(i) + "]", d)) {
                out.error = rd.error;
                return out;
            }
            v.push_back(d);
        }
        c.decals = std::move(v);
    }
    if (const auto it = j.find("decalBlendGroups"); it != j.end()) {
        if (!it->is_array()) {
            out.error = "appearance: 'decalBlendGroups' is not an array";
            return out;
        }
        std::vector<std::optional<repl::CustomizationBlendGroup>> v;
        for (size_t i = 0; i < it->size(); ++i) {
            const json& e = (*it)[i];
            if (e.is_null()) {
                v.push_back(std::nullopt);
                continue;
            }
            repl::CustomizationBlendGroup g;
            if (!rd.read_blend_group(e, "decalBlendGroups[" + std::to_string(i) + "]", g)) {
                out.error = rd.error;
                return out;
            }
            v.push_back(g);
        }
        c.blend_groups = std::move(v);
    }

    for (const auto& [key, dest] : {std::pair<const char*, std::optional<bool>*>{"bIsHelmetVisible", &c.helmet_visible},
                                    std::pair<const char*, std::optional<bool>*>{"bIsCapeVisible", &c.cape_visible}}) {
        if (const auto it = j.find(key); it != j.end()) {
            bool b = false;
            if (!rd.read_bool(*it, key, b)) {
                out.error = rd.error;
                return out;
            }
            *dest = b;
        }
    }

    if (const auto it = j.find("sectionsValues"); it != j.end()) {
        if (!it->is_object()) {
            out.error = "appearance: 'sectionsValues' is not an object";
            return out;
        }
        std::vector<repl::CustomizationSection> v;
        for (const auto& [name, value] : it->items()) {
            const std::string path = "sectionsValues." + name;
            size_t key = kBodySectionCount;
            for (size_t i = 0; i < kBodySectionCount; ++i)
                if (lower_first(kBodySections[i]) == name) key = i;
            if (key == kBodySectionCount) {
                out.error = "appearance: '" + path + "' is not a body section";
                return out;
            }
            if (!value.is_object()) {
                out.error = "appearance: '" + path + "' is not an object";
                return out;
            }
            repl::CustomizationSection s;
            s.key = static_cast<uint8_t>(key);
            const json* x = rd.required(value, "x", path);
            const json* y = rd.required(value, "y", path);
            const json* z = rd.required(value, "z", path);
            if (!x || !y || !z || !rd.read_double(*x, path + ".x", s.value.x) || !rd.read_double(*y, path + ".y", s.value.y) ||
                !rd.read_double(*z, path + ".z", s.value.z)) {
                out.error = rd.error;
                return out;
            }
            v.push_back(s);
        }
        c.sections_values = std::move(v);
    }

    if (const auto it = j.find("faceMorphWeightMaps"); it != j.end()) {
        if (!it->is_object()) {
            out.error = "appearance: 'faceMorphWeightMaps' is not an object";
            return out;
        }
        std::vector<repl::CustomizationMorph> v;
        for (const auto& [name, value] : it->items()) {
            repl::CustomizationMorph m;
            // the capture's morph FNames start with a capital where the client's JSON keys start with a small letter
            m.name = name_from_string(upper_first(name));
            if (!rd.read_float(value, "faceMorphWeightMaps." + name, m.weight)) {
                out.error = rd.error;
                return out;
            }
            v.push_back(std::move(m));
        }
        c.face_morph_weights = std::move(v);
    }

    // race and gender are on the record as well: the two have to agree
    if (const auto it = j.find("race"); it != j.end()) {
        const auto named = it->is_string() ? client_race_from_name(it->get<std::string>()) : std::nullopt;
        if (!named) {
            out.error = "appearance: 'race' is not a race name";
            return out;
        }
        if (*named != race) {
            out.error = std::string("appearance: the appearance is of a ") + client_race_name(*named) + " but the character's race is " + client_race_name(race);
            return out;
        }
    }
    if (const auto it = j.find("gender"); it != j.end()) {
        const auto named = it->is_string() ? client_gender_from_name(it->get<std::string>()) : std::nullopt;
        if (!named) {
            out.error = "appearance: 'gender' is not a gender name";
            return out;
        }
        if (*named != gender) {
            out.error = std::string("appearance: the appearance is ") + client_gender_name(*named) + " but the character's gender is " + client_gender_name(gender);
            return out;
        }
    }
    if (const auto it = j.find("class"); it != j.end() && !it->is_string()) {
        out.error = "appearance: 'class' is not a name";
        return out;
    }
// Cyb : C-O-A

    for (const auto& [key, value] : j.items()) {
        (void)value;
        if (!is_known_key(key)) out.ignored_keys.push_back(key);
    }

    // a value the wire cannot carry (a morph used twice, ...) is refused here, not when the pawn is built
    if (!repl::struct_accepts("FCharacterCustomizationSaveData", repl::Value(c))) {
        out.error = "appearance: the customization has a value the wire cannot carry (a morph or section named twice, or a name that is not text)";
        return out;
    }
    out.value = std::move(c);
    return out;
}

std::optional<std::string> customization_to_json(const repl::CharacterCustomization& c) {
    json j = json::object();
    j["presetGuid"] = c.preset_guid ? std::to_string(static_cast<int64_t>(*c.preset_guid)) : std::string("0");
    j["randomSeed"] = c.random_seed ? static_cast<int64_t>(static_cast<int32_t>(*c.random_seed)) : 0;
    for (const FieldSpec& f : kFields) {
        const auto it = c.deltas.find(f.field);
        if (it == c.deltas.end()) continue;
        switch (f.kind) {
        case Kind::Float: {
            const float v = std::bit_cast<float>(static_cast<uint32_t>(it->second));
            if (!std::isfinite(v)) return std::nullopt;
            j[f.key] = static_cast<double>(v);
            break;
        }
        case Kind::Int32: j[f.key] = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(it->second))); break;
        case Kind::Id: j[f.key] = std::to_string(static_cast<int64_t>(it->second)); break;
        }
    }
    if (c.decals) {
        json a = json::array();
        for (const auto& d : *c.decals) {
            if (!d) {
                a.push_back(nullptr);
                continue;
            }
            json o = json::object();
            o["guid"] = std::to_string(static_cast<int64_t>(d->guid));
            const auto flt = [&](const char* key, uint32_t bits) { o[key] = static_cast<double>(std::bit_cast<float>(bits)); };
            flt("strength", d->strength);
            flt("scaleU", d->scale_u);
            flt("scaleV", d->scale_v);
            o["slotIndex"] = d->slot_index;
            o["mergeSectionMaterialBase"] = std::to_string(static_cast<int64_t>(d->merge_section_material_base));
            flt("offsetU", d->offset_u);
            flt("offsetV", d->offset_v);
            flt("angle", d->angle);
            o["color"] = std::to_string(static_cast<int64_t>(d->color));
            flt("discoloration", d->discoloration);
            a.push_back(std::move(o));
        }
        j["decalData"] = std::move(a);
    }
    if (c.blend_groups) {
        json a = json::array();
        for (const auto& g : *c.blend_groups) {
            if (!g) {
                a.push_back(nullptr);
                continue;
            }
            json o = json::object();
            o["decalAppearanceGuid"] = std::to_string(static_cast<int64_t>(g->decal_appearance_guid));
            o["colorGuid"] = std::to_string(static_cast<int64_t>(g->color_guid));
            o["intensity"] = static_cast<double>(std::bit_cast<float>(g->intensity));
            a.push_back(std::move(o));
        }
        j["decalBlendGroups"] = std::move(a);
    }
    if (c.helmet_visible) j["bIsHelmetVisible"] = *c.helmet_visible;
    if (c.cape_visible) j["bIsCapeVisible"] = *c.cape_visible;
    if (c.race_gender) {
        if (const char* g = client_gender_name(static_cast<ClientGender>(c.race_gender->gender))) j["gender"] = g;
        if (const char* r = client_race_name(static_cast<ClientRace>(c.race_gender->race))) j["race"] = r;
    }
    if (c.face_morph_weights) {
        json o = json::object();
        for (const auto& m : *c.face_morph_weights) {
            const auto name = name_to_string(m.name);
            if (!name) return std::nullopt;
            const float w = std::bit_cast<float>(m.weight);
            if (!std::isfinite(w)) return std::nullopt;
            o[*name] = static_cast<double>(w);
        }
        j["faceMorphWeightMaps"] = std::move(o);
    }
    if (c.sections_values) {
        json o = json::object();
        for (const auto& s : *c.sections_values) {
            const char* name = body_section_name(s.key);
            if (!name) return std::nullopt;
            if (!std::isfinite(s.value.x) || !std::isfinite(s.value.y) || !std::isfinite(s.value.z)) return std::nullopt;
            json v = json::object();
            v["x"] = s.value.x;
            v["y"] = s.value.y;
            v["z"] = s.value.z;
            o[lower_first(name)] = std::move(v);
        }
        j["sectionsValues"] = std::move(o);
    }
    return j.dump();
}

// ---- the character id and the stats -------------------------------------------------------------------------------------
// C | O | A with Cyb

std::optional<std::array<uint32_t, 4>> character_guid_words(std::string_view hex32) {
    if (hex32.size() != 32) return std::nullopt;
    std::array<uint32_t, 4> w{};
    for (size_t i = 0; i < 32; ++i) {
        const int d = hex_digit(hex32[i]);
        if (d < 0) return std::nullopt;
        w[i / 8] = (w[i / 8] << 4) | static_cast<uint32_t>(d);
    }
    return w;
}

// ---- plan D1, stage 2: weapons, the abilities' static parts ---------------------------------------------------------------------------

int64_t general_unstuck() { return captured::kKnownAbilities[0]; }

std::span<const int64_t> static_known_tail() {
    constexpr size_t kTail = 45;   // the fishing abilities, Rest, 33 emotes, Dodge, Guard, Sprint, the dances ... General_DropCrate
    constexpr size_t n = sizeof(captured::kKnownAbilities) / sizeof(captured::kKnownAbilities[0]);
    static_assert(n > kTail, "the captured list holds the tail");
    return std::span<const int64_t>(captured::kKnownAbilities + (n - kTail), kTail);
}

std::optional<WeaponInfo> worn_weapon(const content::ContentDb& content, const items::Inventory& inventory, uint8_t slot, std::string* error) {
    const svc::InventoryItem* it = inventory.at(svc::ItemContainer::Equipment, slot);
    if (!it) return std::nullopt;
    std::string e;
    const auto w = content.item_weapon(it->item_record_id, e);
    if (!w) {
        if (!e.empty() && error) *error = e;
        return std::nullopt;
    }
    if (w->skill_line == 0 || w->attack_ability == 0) return std::nullopt;   // an item with a combo and no weapon line is a tool
    WeaponInfo out;
    out.combo = w->attack_ability;
    out.line = w->skill_line;
    out.line_name = w->skill_line_name;
    return out;
}

std::vector<int64_t> carried_combos(const content::ContentDb& content, const items::Inventory& inventory) {
    std::vector<int64_t> out;
    const std::vector<svc::InventoryItem> all = inventory.items();   // (a copy: items() returns by value)
    for (const svc::ItemContainer c : {svc::ItemContainer::Equipment, svc::ItemContainer::Backpack}) {
        std::vector<const svc::InventoryItem*> held;
        for (const svc::InventoryItem& it : all) {
            if (it.container == c) held.push_back(&it);
        }
        std::sort(held.begin(), held.end(), [](const svc::InventoryItem* a, const svc::InventoryItem* b) { return a->slot < b->slot; });
        for (const svc::InventoryItem* it : held) {
            std::string e;
            const auto w = content.item_weapon(it->item_record_id, e);
            if (!w || w->skill_line == 0 || w->attack_ability == 0) continue;
            if (std::find(out.begin(), out.end(), w->attack_ability) == out.end()) out.push_back(w->attack_ability);
        }
    }
    return out;
}

std::vector<int64_t> carried_item_abilities(const content::MountData* mounts, const items::Inventory& inventory) {
    std::vector<int64_t> out;
    if (!mounts) return out;
    for (const int64_t record : inventory.records()) {
        const content::MountRow* row = mounts->find(record);
        if (!row || row->cast_ability == 0) continue;
        if (std::find(out.begin(), out.end(), row->cast_ability) == out.end()) out.push_back(row->cast_ability);
    }
    return out;
}

namespace {

/// The captured Bard's records with the items' abilities put in before the static tail that ends the list (the way the join's lists do it).
void add_item_records(std::vector<AbilityRecord>& records, std::span<const int64_t> abilities) {
    const std::span<const int64_t> tail = static_known_tail();
    size_t at = records.size();
    if (records.size() >= tail.size()) {
        bool ends_with_tail = true;
        for (size_t i = 0; ends_with_tail && i < tail.size(); ++i) ends_with_tail = records[records.size() - tail.size() + i].guid == tail[i];
        if (ends_with_tail) at = records.size() - tail.size();
    }
    for (const int64_t a : abilities) {
        const bool known = std::any_of(records.begin(), records.end(), [&](const AbilityRecord& r) { return r.guid == a; });
        if (a == 0 || known) continue;
        records.insert(records.begin() + static_cast<std::ptrdiff_t>(at++), AbilityRecord{a, content::kTypeAbilityRecord});
    }
}

}  // namespace

std::vector<stats::ActiveEffect> rank_effects_of(std::span<const content::AutoGrant* const> granted) {
    std::vector<stats::ActiveEffect> out;
    for (const content::AutoGrant* g : granted) {
        if (!g || g->mods.empty()) continue;
        stats::ActiveEffect e;
        e.effect = {stats::kRefTypeEffect, g->effect_guid};
        e.stacks = 1;
        for (const content::RankStatMod& m : g->mods) e.mods.push_back({m.stat_guid, m.value});
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<stats::WornItem> worn_items_of(const items::Inventory& inventory) {
    std::vector<stats::WornItem> out;
    for (const svc::InventoryItem& it : inventory.items()) {
        if (it.container == svc::ItemContainer::Equipment) out.push_back({it.item_record_id, it.quality, it.slot});
    }
    std::sort(out.begin(), out.end(), [](const stats::WornItem& a, const stats::WornItem& b) { return a.slot < b.slot; });
    return out;
}

std::optional<StatsSetup> stats_setup_for(const std::vector<stats::StatValue>& values, const StatsSetup& layout, std::string& error) {
    std::map<int64_t, const stats::StatValue*> by_guid;
    for (const stats::StatValue& v : values) {
        if (!by_guid.emplace(v.guid, &v).second) {
            error = "the stat " + std::to_string(v.guid) + " is in the character's stats twice";
            return std::nullopt;
        }
        if (!std::isfinite(v.value) || !std::isfinite(v.base) || !std::isfinite(v.equip)) {
            error = "the stat " + std::to_string(v.guid) + " is not a finite number";
            return std::nullopt;
        }
    }
    const auto item_of = [](int32_t id, const stats::StatValue& v) {
        StatItem it;
        it.id = id;
        it.record = v.guid;
        it.value = std::bit_cast<uint32_t>(v.value);
        it.extended = v.buckets();
        if (it.extended) {
            it.base = std::bit_cast<uint32_t>(v.base);
            it.equipment = std::bit_cast<uint32_t>(v.equip);
        }
        return it;
    };
    StatsSetup out;
    std::set<int64_t> placed;
    const auto fill = [&](const StatArray& from, uint8_t mask, StatArray& to) {
        int32_t max_id = 0;
        for (const StatItem& it : from.items) max_id = std::max(max_id, it.id);
        for (const StatItem& it : from.items) {   // the layout's items that are still stats of the class, in its order, with its ids
            const auto found = by_guid.find(it.record);
            if (found == by_guid.end() || found->second->replication != mask || found->second->float_array()) continue;
            to.items.push_back(item_of(it.id, *found->second));
            placed.insert(it.record);
        }
        for (const stats::StatValue& v : values) {   // the class's own: the next ids
            if (v.replication != mask || v.float_array() || placed.count(v.guid)) continue;
            to.items.push_back(item_of(++max_id, v));
            placed.insert(v.guid);
        }
        to.array_key = from.array_key;   // the capture's keys are below its highest ids (the server's own history): kept as they are
    };
    fill(layout.owner, 1, out.owner);
    fill(layout.owner_proxy, 5, out.owner_proxy);
    fill(layout.everyone_proxy, 7, out.everyone_proxy);
    // the float array: TempHealth, with the captured item's id
    int32_t max_float_id = 0;
    for (const FloatStatItem& it : layout.float_everyone_proxy) max_float_id = std::max(max_float_id, it.id);
    for (const FloatStatItem& it : layout.float_everyone_proxy) {
        const auto found = by_guid.find(it.record);
        if (found != by_guid.end() && found->second->float_array() && found->second->replication == 7) out.float_everyone_proxy.push_back(it);
    }
    for (const stats::StatValue& v : values) {
        if (!v.float_array() || v.replication != 7) continue;
        const bool have = std::any_of(out.float_everyone_proxy.begin(), out.float_everyone_proxy.end(), [&](const FloatStatItem& f) { return f.record == v.guid; });
        if (!have) out.float_everyone_proxy.push_back({++max_float_id, v.guid});
    }
    out.float_array_key = layout.float_array_key;
    return out;
}
// Cyb.C | O | A

// ---- the setup ----------------------------------------------------------------------------------------------------------

std::vector<HeldItem> held_items_for(const items::Inventory& inventory) {
    const std::array<int64_t, 23> records = items::held_records(inventory);
    std::vector<HeldItem> out;
    out.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        HeldItem h;
        h.id = static_cast<int32_t>(i + 1);
        h.slot = items::kHeldSlots[i];
        h.item_guid = records[i];
        h.enchantment = 0;
        out.push_back(h);
    }
    return out;
}

CharacterSetupResult build_player_setup(const svc::Character& ch, const content::ContentDb& content, const LoginIdentity& login,
                                        const SessionValues& session, const StatContext& stat_context) {
    CharacterSetupResult out;
    const auto fail = [&](std::string message, SetupRefusal refusal = SetupRefusal::Other) {
        out.setup.reset();
        out.error = std::move(message);
        out.refusal = refusal;
        return out;
    };

    PlayerSetup s;

    // the character [char]
    const auto words = character_guid_words(ch.id);
    if (!words) return fail("the character id '" + ch.id + "' is not 32 hex digits");
    s.character_guid = *words;
    if (ch.name.empty()) return fail("the character record has no name");
    if (!repl::fstring_fits(ch.name)) return fail("the character's name cannot be sent (not UTF-8 text, a NUL, or too long)");
    s.name = ch.name;

    const auto race = client_race_from_id(ch.race_id);
    if (!race) {
        return fail("race id " + std::to_string(ch.race_id) + " is not a playable race (0 Kaelar to 7 Nikua; 8 Tulnar cannot be created)",
                    SetupRefusal::UnsupportedRace);
    }
    const auto gender = client_gender_from_id(ch.gender_id);
    if (!gender) return fail("gender id " + std::to_string(ch.gender_id) + " is not 0 (Male) or 1 (Female)");
    s.race = static_cast<uint8_t>(*race);
    s.gender = static_cast<uint8_t>(*gender);

    if (ch.level < content::kMinLevel || ch.level > content::kMaxLevel) {
        return fail("the character's level " + std::to_string(ch.level) + " is outside content.db's levels " +
                    std::to_string(content::kMinLevel) + " to " + std::to_string(content::kMaxLevel));
    }
    s.level = ch.level;

    // the class (ECharacterClass, looked up by its name) and the six primary attributes [C1]
    const char* class_name = client_class_name(ch.class_id);
    if (!class_name) {
        return fail("class id " + std::to_string(ch.class_id) + " is not a class (1 Ranger to 8 Mage)" +
                    (ch.class_id == 0 ? ": the character service could not derive it for this record" : ""));
    }
    std::string content_error;
    const auto cls = content.class_for_name(class_name, content_error);
    if (!cls) return fail(content_error);
    if (!ch.class_name.empty() && ch.class_name != cls->name) {
        out.notes.push_back("the record's class name '" + ch.class_name + "' is not '" + cls->name + "', the name of class " +
                            std::to_string(ch.class_id) + ": the class id decides");
    }
    if (ch.archetype_id != 0 && ch.archetype_id != cls->archetype_id) {
        out.notes.push_back("the record's archetype id " + std::to_string(ch.archetype_id) + " is not " + std::to_string(cls->archetype_id) +
                            ", the archetype of " + cls->name + ": the class id decides");
    }
    s.class_record_guid = cls->guid;
    if (!stat_context.bundle || !stat_context.gear) return fail("worldd has no stat program: the character's stats cannot be computed");

    // the abilities: the captured Bard's lists whatever the class (spec section 5)
    s.known_abilities = captured_bard_known_abilities();
    s.records_to_load = captured_bard_records_to_load();
// (CoA ~ C_Y_B)

    // the items (plan B2): what is in the bag, what is worn, and what that shows
    std::string items_error;
    auto inventory = items::Inventory::from_items(ch.items, items_error);
    if (!inventory) return fail("the character's items cannot be played: " + items_error);
    for (const svc::InventoryItem& it : inventory->items()) {
        StoredItem stored;
        stored.storage = it.container == svc::ItemContainer::Backpack ? ItemStorage::Backpack
                         : it.container == svc::ItemContainer::Equipment ? ItemStorage::Equipment
                                                                         : ItemStorage::Quest;
        stored.slot = it.slot;
        stored.item = items::to_instance(it);
        s.items.push_back(std::move(stored));
    }
    if (inventory->count(svc::ItemContainer::Equipment) > 0) {
        s.held_items.array_key = kInitialHeldItemsKey;
        s.held_items.items = held_items_for(*inventory);
    }
    s.appearance_ids = items::appearance_ids(*inventory);
    // a note says what the character plays with (a character with no items has nothing to say: the session warns about it)
    if (inventory->size() > 0) {
        out.notes.push_back("items: " + std::to_string(inventory->count(svc::ItemContainer::Backpack)) + " in the bag, " +
                            std::to_string(inventory->count(svc::ItemContainer::Equipment)) + " worn, " +
                            std::to_string(inventory->count(svc::ItemContainer::Quest)) + " quest items");
    }
    // the progress (plan D1): the record's level and its Adventuring XP row; the ranks the level has granted and their stat mods
    std::vector<const content::AutoGrant*> granted;
    if (stat_context.curve) {
        std::optional<int64_t> xp;
        int64_t debt = 0;
        for (const svc::Experience& e : ch.experience) {
            if (e.leveling_type_guid == content::kGuidAdventuring) {
                xp = e.xp;
                debt = e.debt;
            }
        }
        out.progress = leveling::progress_of(*stat_context.curve, ch.level, xp, debt);
        if (xp && *xp < stat_context.curve->xp_for(ch.level)) {
            out.notes.push_back("the record's Adventuring XP " + std::to_string(*xp) + " is below level " + std::to_string(ch.level) + "'s threshold: the threshold is used");
        }
        if (stat_context.grants) granted = leveling::granted_by(*stat_context.grants, out.progress.level);
        s.progress.xp = out.progress.xp;
        s.progress.debt = out.progress.debt;
        if (!stat_context.talents && !granted.empty()) {
            TreeSetup universal;
            universal.tree = content::kGuidUniversalTree;
            for (const content::AutoGrant* g : granted) universal.ranks.push_back(g->rank_guid);
            s.progress.trees.push_back(std::move(universal));
        }
        if (!granted.empty()) out.notes.push_back(std::to_string(granted.size()) + " rank(s) of the Universal tree granted by the level");
    } else {
        out.progress.level = ch.level;   // no curve (a test's context): the tracker shows 0 XP
    }
// [C Y B + CoA]

    // the talents (plan D1, stage 2): the ranks the record stored under the catalog's rules, what the XPTracker lists, the abilities the ranks and
    // the weapon in hand make, and the action bar
    if (stat_context.talents) {
        talents::Book book(*stat_context.talents, cls->guid, out.progress.level);
        size_t dropped = 0;
        for (const svc::TreeRanks& t : ch.skill_ranks) book.load(t.tree_guid, t.rank_guids, &dropped);
        if (dropped > 0) {
            out.notes.push_back(std::to_string(dropped) + " stored rank(s) are no rank of a tree the character can have (content.db changed?): left out");
        }
        s.progress.trees.clear();
        for (const talents::Tree* t : book.listed_trees()) s.progress.trees.push_back({t->guid, book.list(*t)});
        std::string weapon_error;
        const auto primary = worn_weapon(content, *inventory, items::equip::Primary, &weapon_error);
        if (!weapon_error.empty()) return fail(weapon_error);
        if (primary) {
            s.weapon_skill_line.guid = primary->line;
            s.weapon_skill_line.name = primary->line_name;
            out.wielded_line = primary->line;
        }
        const std::vector<int64_t> item_abilities = carried_item_abilities(stat_context.mounts, *inventory);
        const talents::AbilityLists lists = talents::join_abilities(book, general_unstuck(), primary ? primary->combo : 0, carried_combos(content, *inventory),
                                                                    static_known_tail(), item_abilities);
        s.known_abilities = lists.known;
        s.records_to_load.clear();
        for (const int64_t g : lists.records) s.records_to_load.push_back({g, content::kTypeAbilityRecord});
        // the bar: what the character arranged, or the weapon bank and the class's auto-granted skills (the first empty slots of the class pages)
        if (!ch.action_bar.empty()) {
            for (const svc::ActionbarSlot& a : ch.action_bar) {
                // the narrowing casts below wrap: a stored value outside what the wire holds is left out, not turned into another one
                if (a.use_type < 0 || a.use_type > 255 || a.instigator < 0 || a.instigator > 255 || a.item_rarity < 0 || a.item_rarity > 255 ||
                    a.combo_button < 0 || a.combo_button > 255) {
                    out.notes.push_back("a stored action bar slot is left out: a value is outside the byte the wire holds");
                    continue;
                }
                repl::ActionbarSlot slot;
                slot.bank_index = a.bank;
                slot.slot_index = a.slot;
                slot.use.use_type = static_cast<uint8_t>(a.use_type);
                slot.use.instigator = static_cast<uint8_t>(a.instigator);
                slot.use.ability_id = a.ability_id;
                slot.use.skill_rank_id = a.skill_rank_id;
                slot.use.pet_command_id = a.pet_command_id;
                slot.use.item_record_id = a.item_record_id;
                if (a.item_guid.size() == 32) {
                    uint32_t w[4] = {0, 0, 0, 0};
                    bool good = true;
                    for (size_t i = 0; i < 32; ++i) {
                        const int d = hex_digit(a.item_guid[i]);
                        good = good && d >= 0;
                        w[i / 8] = (w[i / 8] << 4) | static_cast<uint32_t>(d < 0 ? 0 : d);
                    }
                    if (good) slot.use.item_guid = repl::GuidValue{w[0], w[1], w[2], w[3]};
                }
                slot.use.item_rarity = static_cast<uint8_t>(a.item_rarity);
                slot.use.item_quality = a.item_quality;
                slot.use.combo_button = static_cast<uint8_t>(a.combo_button);
                std::string why;
                if (!out.action_bar.set(slot, &why)) out.notes.push_back("a stored action bar slot is left out: " + why);
            }
        } else {
            out.action_bar = actionbar::Bar::defaults({content::kGuidGuard, content::kGuidSprint, content::kGuidDodge});
            if (const talents::Tree* ct = book.class_tree()) {
                for (const talents::RankRef& r : book.listed(*ct)) {
                    if (r.node->auto_hotbar && r.rank->ability != 0) out.action_bar.place_skill(r.rank->ability, r.rank->guid);
                }
            }
        }
        out.book = std::move(book);
    } else {
        // no talents catalog (a test's context): the captured Bard's records, and the abilities of the items carried before their static tail
        add_item_records(s.records_to_load, carried_item_abilities(stat_context.mounts, *inventory));
    }

    // the stats (plan B3): the class at its level wearing what it wears, by the stat program; the captured Bard's ids, keys and order lay them out
    {
        stats::CharacterStats character_stats(*stat_context.bundle, *stat_context.gear, ch.class_id, ch.level, stat_context.rounding);
        // plan D1: the granted ranks' constant mods (Endurance Training: MaxStamina +10); stage 2: every rank the character has
        // (plan D2: through the effect state, the one path for every stat mod; the ranks held are what GetSkillPurchased reads)
        if (out.book) {
            character_stats.set_passives(out.book->passives(out.wielded_line));
            character_stats.set_ranks(out.book->held_ranks());
        } else {
            character_stats.set_passives(rank_effects_of(granted));
        }
        const std::vector<stats::WornItem> worn = worn_items_of(*inventory);
        std::string stats_error;
        if (!character_stats.start(worn, stats_error)) return fail("the character's stats cannot be computed: " + stats_error);
        if (character_stats.faults() != 0) {
            return fail("the stat program faulted " + std::to_string(character_stats.faults()) + " time(s) for this character (a division by zero or a bad curve read)");
        }
        auto layout = stats_setup_for(character_stats.values(), captured_bard_level1_stats(), stats_error);
        if (!layout) return fail("the character's stats cannot be laid out: " + stats_error);
        s.stats = std::move(*layout);
        if (character_stats.unknown_items() != 0) {
            out.notes.push_back(std::to_string(character_stats.unknown_items()) + " worn item(s) are not in content.db: they add nothing to the stats");
        }
        if (character_stats.unknown_sets() != 0) {
            out.notes.push_back(std::to_string(character_stats.unknown_sets()) + " set bonus(es) of the worn items are not in content.db: they add nothing");
        }
        out.character_stats = std::move(character_stats);
    }
    out.inventory = std::move(inventory);

    // the appearance
    const CustomizationFromJson cust = customization_from_json(ch.appearance_json, *race, *gender);
    if (!cust.value) return fail(cust.error);
    s.customization = *cust.value;
    for (const std::string& key : cust.ignored_keys) out.notes.push_back("the appearance has a member '" + key + "' this server does not read");

    // the spawn
    if (ch.has_position) {
        const svc::Position& p = ch.position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.yaw)) {
            return fail("the character's saved position is not finite");
        }
        s.x = p.x;
        s.y = p.y;
        s.z = p.z;
        s.yaw_degrees = p.yaw;
    } else {
        s.x = kDefaultSpawn.x;
        s.y = kDefaultSpawn.y;
        s.z = kDefaultSpawn.z;
        s.yaw_degrees = kDefaultSpawn.yaw_degrees;
        out.notes.push_back("the character has no saved position: it spawns at the default spawn point");
    }

    // the login and the session [sess]
    s.login = login;
    s.server_id = session.server_id;
    s.world_time = session.world_time;
    s.player_id = session.player_id;
    s.guids = session.guids;

    out.setup = std::move(s);
    return out;
}

}  // namespace aoc::worldd::bootstrap

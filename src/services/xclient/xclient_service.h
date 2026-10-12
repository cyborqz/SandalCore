// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "ics_xclient.grpc.pb.h"
#include "util/proxy_logger.h"

// â”€â”€â”€ UUID generation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static std::string generate_msg_uuid() {
    static std::mt19937 rng(std::random_device{}());
    static const char hex[] = "0123456789abcdef";
    std::uniform_int_distribution<> dist(0, 15);
    std::string uuid;
    uuid.reserve(32);
    for (int i = 0; i < 32; ++i) uuid += hex[dist(rng)];
    return uuid;
}

// â”€â”€â”€ Diagnostics â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static std::string hex_dump(const std::string& data, size_t max_bytes = 512) {
    std::ostringstream oss;
    size_t len = std::min(data.size(), max_bytes);
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << (static_cast<unsigned>(data[i]) & 0xFF);
        if (i + 1 < len) oss << ' ';
    }
    if (data.size() > max_bytes) oss << " ... (" << data.size() << " bytes total)";
    return oss.str();
}

static void log_system_data(const std::string& label,
                            const ics_common::MessageWrapper& w) {
    if (!w.has_system_data()) {
        spdlog::debug("[XClient] {} system_data: <absent>", label);
        return;
    }
    const auto& sd = w.system_data();
    std::ostringstream oss;
    oss << "tags={";
    for (const auto& kv : sd.tags())
        oss << kv.first << ":" << kv.second << ", ";
    oss << "} trace_samples=" << sd.trace_samples_size();
    spdlog::info("[XClient] {} system_data: {}", label, oss.str());
}

// â”€â”€â”€ Wrapper builder â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// KEY: echo system_data from request (msg_id correlation).

static ics_common::MessageWrapper make_response(
    const std::string& type_name,
    const std::string& payload,
    const ics_common::MessageWrapper* request,
    ics_common::IcsStatusCode status = ics_common::ICS_STATUS_CODE_SUCCESS)
{
    ics_common::MessageWrapper w;
    // Echo system_data from request for msg_id correlation
    if (request && request->has_system_data())
        *w.mutable_system_data() = request->system_data();
    w.set_status_code(status);
    w.set_message_type_name(type_name);
    w.set_message_data(payload); // Always set, even if empty
    return w;
}
// [C-O-A - Cyb]

// Server-push (no originating request)
static ics_common::MessageWrapper make_push(
    const std::string& type_name,
    const std::string& payload,
    ics_common::IcsStatusCode status = ics_common::ICS_STATUS_CODE_SUCCESS)
{
    ics_common::MessageWrapper w;
    w.set_status_code(status);
    w.set_message_type_name(type_name);
    w.set_message_data(payload);
    return w;
}

// â”€â”€â”€ World config â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

struct WorldConfig {
    std::string world_id           = "aoc-emu-useast2-local";
    std::string world_display_name = "[US] Local Server";
    bool        available          = true;
    int32_t     fullness           = 0;   // 0 = normal (real server uses 0)
    int32_t     queue_job_count_hi = 0;
    int32_t     character_count    = 0;
};

/// Default world list â€” single local emulation server
static std::vector<WorldConfig> make_default_worlds() {
    return {
        {"aoc-emu-useast2-local", "[EMU] Local Server", true, 0, 0, 0},
    };
}

// â”€â”€â”€ Protobuf wire encoding (no .proto available for aoc_character) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void pb_encode_varint(std::string& out, uint64_t val) {
    do {
        uint8_t byte = val & 0x7f;
        val >>= 7;
        if (val) byte |= 0x80;
        out += static_cast<char>(byte);
    } while (val);
}

static void pb_encode_tag(std::string& out, uint32_t field_num, uint32_t wire_type) {
    pb_encode_varint(out, (static_cast<uint64_t>(field_num) << 3) | wire_type);
}

static void pb_encode_string(std::string& out, uint32_t field_num, const std::string& val) {
    if (val.empty()) return;
    pb_encode_tag(out, field_num, 2);
    pb_encode_varint(out, val.size());
    out += val;
}

static void pb_encode_uint64(std::string& out, uint32_t field_num, uint64_t val) {
    if (val == 0) return;
    pb_encode_tag(out, field_num, 0);
    pb_encode_varint(out, val);
}

static void pb_encode_bytes(std::string& out, uint32_t field_num, const std::string& val) {
    if (val.empty()) return;
    pb_encode_tag(out, field_num, 2);
    pb_encode_varint(out, val.size());
    out += val;
}

// â”€â”€â”€ In-memory character store â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

struct StoredCharacter {
    std::string character_id;       // 32-char hex (server-generated)
    std::string character_name;
    std::string world_id;
    std::string create_info_raw;    // CreateCharacterInfo proto bytes from client
    uint64_t    created_at_ms;      // epoch milliseconds
    uint64_t    archetype_id = 0;   // Extracted from CreateCharacterInfo
    uint64_t    race_id      = 0;   // Extracted from CreateCharacterInfo
    uint64_t    gender_id    = 0;   // Extracted from CreateCharacterInfo
    uint32_t    level        = 1;   // Default level for new characters
    std::string class_name;         // e.g. "Mage", "Fighter" - from customization JSON
    std::string race_name;          // e.g. "Dunir", "Empyrean" - from customization JSON
    std::string gender_name;        // e.g. "Male", "Female" - from customization JSON
};

// â”€â”€â”€ Raw proto field decoder â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

struct RawProtoField {
    uint32_t    field_num;
    uint32_t    wire_type;
    uint64_t    varint_val   = 0;  // wire_type 0
    std::string bytes_val;         // wire_type 2
    uint64_t    fixed64_val  = 0;  // wire_type 1
    uint32_t    fixed32_val  = 0;  // wire_type 5
};

static std::vector<RawProtoField> decode_raw_proto(const std::string& data) {
    std::vector<RawProtoField> fields;
    size_t pos = 0;
    auto read_varint = [&](uint64_t& out) -> bool {
        out = 0; int shift = 0;
        while (pos < data.size()) {
            uint8_t b = static_cast<uint8_t>(data[pos++]);
            out |= static_cast<uint64_t>(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) return true;
            if (shift >= 64) return false;
        }
        return false;
    };
    while (pos < data.size()) {
        uint64_t tag;
        if (!read_varint(tag)) break;
        RawProtoField f{};
        f.field_num = static_cast<uint32_t>(tag >> 3);
        f.wire_type = static_cast<uint32_t>(tag & 7);
        if (f.field_num == 0) break;
        switch (f.wire_type) {
        case 0: if (!read_varint(f.varint_val)) return fields; break;
        case 1:
            if (pos + 8 > data.size()) return fields;
            std::memcpy(&f.fixed64_val, &data[pos], 8); pos += 8;
            break;
        case 2: {
            uint64_t len;
            if (!read_varint(len) || pos + len > data.size()) return fields;
            f.bytes_val = data.substr(pos, static_cast<size_t>(len));
            pos += static_cast<size_t>(len);
            break;
        }
        case 5:
            if (pos + 4 > data.size()) return fields;
            std::memcpy(&f.fixed32_val, &data[pos], 4); pos += 4;
            break;
        default: return fields;
        }
        fields.push_back(std::move(f));
    }
    return fields;
}
// -- C.O.A & Cyb --

// Log all fields from a raw proto message
static void log_raw_proto(const std::string& label, const std::string& data, size_t max_fields = 30) {
    auto fields = decode_raw_proto(data);
    spdlog::info("[XClient] {} decoded {} fields from {}B:", label, fields.size(), data.size());
    size_t count = 0;
    for (const auto& f : fields) {
        if (++count > max_fields) {
            spdlog::info("[XClient]   ... ({} more fields)", fields.size() - max_fields);
            break;
        }
        switch (f.wire_type) {
        case 0: spdlog::info("[XClient]   field {:3d} (varint)  = {}", f.field_num, f.varint_val); break;
        case 1: spdlog::info("[XClient]   field {:3d} (fixed64) = {}", f.field_num, f.fixed64_val); break;
        case 2: {
            // Try to decode as string
            bool is_text = f.bytes_val.size() > 0 && f.bytes_val.size() < 200;
            if (is_text) {
                for (unsigned char c : f.bytes_val) {
                    if (c < 32 && c != '\n' && c != '\r' && c != '\t') { is_text = false; break; }
                    if (c > 126) { is_text = false; break; }
                }
            }
            if (is_text)
                spdlog::info("[XClient]   field {:3d} (string, {:4d}B) = \"{}\"", f.field_num, f.bytes_val.size(),
                             f.bytes_val.size() <= 80 ? f.bytes_val : f.bytes_val.substr(0, 77) + "...");
            else
                spdlog::info("[XClient]   field {:3d} (bytes,  {:4d}B) hex: {}", f.field_num, f.bytes_val.size(),
                             hex_dump(f.bytes_val, 64));
            break;
        }
        case 5: spdlog::info("[XClient]   field {:3d} (fixed32) = {}", f.field_num, f.fixed32_val); break;
        }
    }
}

// â”€â”€â”€ Archetype ID mapping â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Taken from a capture: Hernan (Tank) had field 2 = 17754.
// AoC has 8 primary archetypes. If Tank (alphabetically last = index 7) = 17754,
// then base = 17754 - 7 = 17747, giving us:
//   Bard=17747, Cleric=17748, Fighter=17749, Mage=17750,
//   Ranger=17751, Rogue=17752, Summoner=17753, Tank=17754

static constexpr uint64_t ARCHETYPE_BASE = 17747;

static uint64_t archetype_name_to_id(const std::string& name) {
    // Alphabetical order (0-indexed)
    if (name == "Bard")     return ARCHETYPE_BASE + 0;  // 17747
    if (name == "Cleric")   return ARCHETYPE_BASE + 1;  // 17748
    if (name == "Fighter")  return ARCHETYPE_BASE + 2;  // 17749
    if (name == "Mage")     return ARCHETYPE_BASE + 3;  // 17750
    if (name == "Ranger")   return ARCHETYPE_BASE + 4;  // 17751
    if (name == "Rogue")    return ARCHETYPE_BASE + 5;  // 17752
    if (name == "Summoner") return ARCHETYPE_BASE + 6;  // 17753
    if (name == "Tank")     return ARCHETYPE_BASE + 7;  // 17754
    spdlog::warn("[XClient] Unknown archetype name '{}', defaulting to Fighter", name);
    return ARCHETYPE_BASE + 2;  // Fighter as fallback
}

// â”€â”€â”€ Customization extraction from CreateCharacterInfo raw bytes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Search for the customization JSON inside create_character_info_raw.
// Real server stores it as the value of key "CustomizationDataKey" in field 100.
// The client sends the same JSON embedded in CreateCharacterInfo.
static std::string extract_customization_json(const std::string& raw) {
    // Look for the distinctive JSON pattern that starts customization data
    static const std::string needle = "{\"presetGuid\":";
    auto pos = raw.find(needle);
    if (pos == std::string::npos) {
        // Fallback patterns
        pos = raw.find("{\"preset");
        if (pos == std::string::npos) return "";
    }

    // Find matching closing brace (handle nested objects)
    int depth = 0;
    size_t end = pos;
    for (; end < raw.size(); ++end) {
        char ch = raw[end];
        if (ch == '{') ++depth;
        else if (ch == '}') {
            --depth;
            if (depth == 0) { ++end; break; }
        }
    }
    if (depth != 0) return "";  // Malformed JSON

    return raw.substr(pos, end - pos);
}

// Extract a JSON string value by key from a raw JSON string.
// Simple parser - looks for "key":"value" pattern.
static std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}
// [Cyb + C | O | A]

// Extract archetype/race/gender from the customization JSON inside CreateCharacterInfo.
// The client embeds class, race, and gender as string fields in the JSON:
//   "gender":"Male", "race":"Kaelar", "class":"Bard"
static void extract_create_info(const std::string& raw, StoredCharacter& sc) {
    log_raw_proto("CreateCharacterInfo", raw, 50);

    // Extract the customization JSON (same function used for field 100)
    std::string json = extract_customization_json(raw);
    if (json.empty()) {
        spdlog::warn("[XClient] Could not find customization JSON in CreateCharacterInfo");
        return;
    }

    // Parse class, race, gender from JSON strings
    std::string class_name  = extract_json_string(json, "class");
    std::string race_name   = extract_json_string(json, "race");
    std::string gender_name = extract_json_string(json, "gender");

    spdlog::info("[XClient] Parsed from JSON: class='{}' race='{}' gender='{}'",
                 class_name, race_name, gender_name);

    // Store string names for logging and future use
    sc.class_name  = class_name;
    sc.race_name   = race_name;
    sc.gender_name = gender_name;

    // Store archetype ID from class name (for potential future use)
    if (!class_name.empty()) {
        sc.archetype_id = archetype_name_to_id(class_name);
        spdlog::info("[XClient] Mapped class '{}' to archetype_id={}", class_name, sc.archetype_id);
    }
}

// Extract the raw bytes of a specific field from a protobuf message.
// Returns the first length-delimited (wire_type 2) value for `target_field`.
static std::string extract_proto_field_raw(const std::string& data, uint32_t target_field) {
    auto fields = decode_raw_proto(data);
    for (const auto& f : fields) {
        if (f.field_num == target_field && f.wire_type == 2)
            return f.bytes_val;
    }
    return "";
}

// â”€â”€â”€ Character info field mapping â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Taken from a capture (2026-06-18, release-global / Lyneth).
//
// *** TOP-LEVEL Character proto (used in GetCharactersReply.characters_raw): ***
//   field 2   (varint) = XP/adventure points (17766 for lvl25 Hernan, 35 for lvl3 PhaedraLux)
//   field 5   (string) = character_id  (32-char hex UUID, no dashes)
//   field 6   (string) = character_name
//   field 7   (string) = world_id
//   field 10  (varint) = created_at    (epoch milliseconds)
//   field 11  (varint) = last_played   (epoch milliseconds)
//   field 100 (bytes)  = CharacterData submessage (customization + stats)
//   field 101 (bytes)  = EquipmentData submessage (equipped items)
//
// *** CharacterData (field 100) sub-fields: ***
//   field100.field1 (message) = customization wrapper:
//     field1.field2 (message) = map entry:
//       field1.field2.field1 (string) = "CustomizationDataKey"
//       field1.field2.field2 (string) = JSON blob (~5-7KB)
//   field100.field2  (varint) = LEVEL (25 for Hernan, 3 for PhaedraLux, 1 for new char)
//   field100.field4  (varint) = unknown (seen: 1)  [only in GetChars, NOT in CreateReply]
//   field100.field5  (varint) = archetype/class ID (6=Hernan?, 4=PhaedraLux?)
//   field100.field6  (varint) = race ID (7=Dunir, 8=Empyrean, 2=Kaelar)
//   field100.field9  (varint) = 1 (constant, all chars)
//   field100.field11 (varint) = unix timestamp? (1769540125 - optional)
//   field100.field12 (varint) = 30 (level cap?)
//   field100.field13 (varint) = 1 (optional, only on some chars)
//   field100.field14 (varint) = 2 (constant - maybe game version?)
//
// *** CreateCharacterRequest.create_character_info_raw fields: ***
//   field1 (message) = customization (same nested structure as field100.field1)
//   field4 (varint) = race ID (2 = Kaelar in our capture)
//   field5 (string) = starting zone name ("Riverlands")
//
// *** CreateCharacterReply.character_info_raw fields: ***
//   field1 (message) = customization (echoed back from request)
//   field2 (varint) = gender (1 = Male)
//   field6 (varint) = race ID (2 = Kaelar)
//   field9 (varint) = 1 (constant)
//   field14 (varint) = 2 (constant)
//   NOTE: NO level, NO field4, NO field5, NO field12, NO field13
//
// *** EquipmentData (field 101) sub-fields: ***
//   field1 (string) = character_id
//   field2 (message, repeated) = equipment slots:
//     field1 (varint) = slot_id (1-79+)
//     field2 (varint) = item_asset_id (large uint64)
//     field4 (varint) = quality/rarity
//     field5 (varint) = enhancement_level
//     field9 (fixed32) = item_level (float: 16.0, 39.0, etc.)
//     field10 (string) = item_guid (32-char hex)
//     field11 (string) = date acquired ("2026.01.25-05.05.45")
//     field13 (string) = "00000000000000000000000000000000"
//     field18 (varint) = 1
//
// *** KEY DIFFERENCE: CreateCharacterRequest vs GetCharactersReply ***
//   The client sends: field1=customization, field4=race, field5=zone_name
//   The server stores and returns in field100: field1=customization, field2=level,
//     field5=archetype, field6=race, field9=1, field12=30, field14=2
//   The CreateCharacterReply returns: field1=customization, field2=gender,
//     field6=race, field9=1, field14=2
//   These are DIFFERENT field layouts for the SAME conceptual data!

// Build the character_info_raw for CreateCharacterReply.
// This is what the server sends back immediately after character creation.
// It has a DIFFERENT field layout than GetCharactersReply field100!
static std::string build_create_reply_info(const StoredCharacter& c) {
    std::string result;

    // field1: customization wrapper (extract from create_info_raw field1)
    if (!c.create_info_raw.empty()) {
        std::string cust_field = extract_proto_field_raw(c.create_info_raw, 1);
        if (!cust_field.empty()) {
            pb_encode_bytes(result, 1, cust_field);
        }
    }

    // field2: gender enum (extract from customization JSON)
    // Map: Male=1, Female=2 (observed: server returned 1 for Male char)
    {
        uint64_t gender_val = 1; // default Male
        if (c.gender_name == "Female") gender_val = 2;
        pb_encode_uint64(result, 2, gender_val);
    }
// [C.Y.B :: C | O | A]

    // field6: race ID (from create_info_raw field4, or from JSON)
    {
        uint64_t race_val = 2; // default Kaelar
        // Try to extract from create_info_raw field4
        auto fields = decode_raw_proto(c.create_info_raw);
        for (const auto& f : fields) {
            if (f.field_num == 4 && f.wire_type == 0) {
                race_val = f.varint_val;
                break;
            }
        }
        pb_encode_uint64(result, 6, race_val);
    }

    // field9: constant 1
    pb_encode_uint64(result, 9, 1);

    // field14: constant 2
    pb_encode_uint64(result, 14, 2);

    spdlog::info("[XClient] Built CreateReply info: {}B", result.size());
    return result;
}

// Build aoc_character.Character protobuf bytes for GetCharactersReply.
// This builds the full character proto with field100 (CharacterData) and
// field101 (EquipmentData) matching the real server format.
static std::string build_character_proto(const StoredCharacter& c) {
    std::string result;

    // field2: XP/adventure points
    // Real server: 17766 for lvl25, 35 for lvl3. Appears to be total XP.
    // For new chars at level 1, we use a small value.
    pb_encode_uint64(result, 2, c.level);

    // fields 5-7: identity
    pb_encode_string(result, 5, c.character_id);
    pb_encode_string(result, 6, c.character_name);
    pb_encode_string(result, 7, c.world_id);

    // fields 10-11: timestamps (epoch millis)
    pb_encode_uint64(result, 10, c.created_at_ms);
    pb_encode_uint64(result, 11, c.created_at_ms);

    // â”€â”€ field 100: CharacterData submessage â”€â”€
    // We rebuild this from scratch to match the real server's field layout.
    // The create_info_raw has: field1=customization, field4=race, field5=zone
    // But field100 needs:      field1=customization, field2=level, field5=archetype,
    //                          field6=race, field9=1, field12=30, field14=2
    {
        std::string f100;

        // field100.1: customization wrapper (extract from create_info_raw)
        if (!c.create_info_raw.empty()) {
            std::string cust_field = extract_proto_field_raw(c.create_info_raw, 1);
            if (!cust_field.empty()) {
                pb_encode_bytes(f100, 1, cust_field);
                spdlog::info("[XClient] field100.1 customization: {}B", cust_field.size());
            }
        }

        // field100.2: LEVEL
        pb_encode_uint64(f100, 2, c.level);

        // field100.5: archetype/class ID
        // We use the archetype_id which was mapped from class name
        if (c.archetype_id > 0) {
            pb_encode_uint64(f100, 5, c.archetype_id);
        }
// C O A captures, C_Y_B code

        // field100.6: race ID (from create_info_raw field4)
        {
            uint64_t race_val = 2; // default
            if (!c.create_info_raw.empty()) {
                auto fields = decode_raw_proto(c.create_info_raw);
                for (const auto& f : fields) {
                    if (f.field_num == 4 && f.wire_type == 0) {
                        race_val = f.varint_val;
                        break;
                    }
                }
            }
            pb_encode_uint64(f100, 6, race_val);
        }

        // field100.9: constant 1
        pb_encode_uint64(f100, 9, 1);

        // field100.12: level cap (30)
        pb_encode_uint64(f100, 12, 30);

        // field100.14: constant 2
        pb_encode_uint64(f100, 14, 2);

        pb_encode_bytes(result, 100, f100);
    }

    // â”€â”€ field 101: EquipmentData submessage â”€â”€
    // New characters have no equipment, but we still send the wrapper
    // with the character_id (matching real server format).
    {
        std::string f101;
        // field101.1: character_id
        pb_encode_string(f101, 1, c.character_id);
        // field101.2: empty repeated equipment (no items for new char)
        pb_encode_bytes(result, 101, f101);
    }

    spdlog::info("[XClient] Built Character proto: {}B (name='{}', level={}, class='{}', race='{}', gender='{}')",
                 result.size(), c.character_name, c.level,
                 c.class_name, c.race_name, c.gender_name);

    return result;
}

// â”€â”€â”€ XClientService â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

class XClientServiceImpl final
    : public ics_xclient::XClientService::Service {
public:
    /// Normal emulation mode
    explicit XClientServiceImpl(std::vector<WorldConfig> worlds = {})
        : worlds_(std::move(worlds))
    {
        if (worlds_.empty()) worlds_ = make_default_worlds();
        load_characters();  // Phase B: restore persisted characters from disk
    }

    /// Set the game server address returned in PlayReply
    void set_game_address(const std::string& ip, int port) {
        game_addr_ = ip + ":" + std::to_string(port);
        spdlog::info("[XClient] Game address set to {}", game_addr_);
    }

    /// Link to GameServer for dynamic relay activation in proxy mode
    void set_relay_callback(std::function<bool(const std::string&)> cb) {
        relay_callback_ = std::move(cb);
    }

    /// Return the name of the most recently created/selected character,
    /// or empty string if none.  Thread-safe read.  Used by GameServer
    /// to patch the replay with the live player's chosen name.
    ///
    /// PM38 (2026-04-30): now returns the LIVE player's character — i.e.,
    /// whichever character was chosen via on_play() — rather than the
    /// most-recently-created character.  This is correct for multi-
    /// character accounts.  Falls back to characters_.back() if no
    /// play() has happened yet (preserves prior single-char behaviour).
    std::string last_character_name() const {
        std::lock_guard<std::mutex> lk(chars_mutex_);
        if (!current_play_character_id_.empty()) {
            for (const auto& c : characters_) {
                if (c.character_id == current_play_character_id_)
                    return c.character_name;
            }
        }
        if (characters_.empty()) return "";
        return characters_.back().character_name;
    }
// built with C-O-A and C_Y_B

    /// Return the archetype_id (class ID) of the most recently created /
    /// selected character, or 0 if none.  Thread-safe read.  Used by
    /// GameServer to patch the captured replay's class field.
    ///
    /// PM38 (2026-04-30): same fix as last_character_name above.
    uint64_t last_character_archetype_id() const {
        std::lock_guard<std::mutex> lk(chars_mutex_);
        if (!current_play_character_id_.empty()) {
            for (const auto& c : characters_) {
                if (c.character_id == current_play_character_id_)
                    return c.archetype_id;
            }
        }
        if (characters_.empty()) return 0;
        return characters_.back().archetype_id;
    }

    /// PD2.3 (2026-05-05) — Return the customization JSON blob for the
    /// currently selected character (or last-created if no Play has fired
    /// yet).  Returns empty string if no character exists or no JSON could
    /// be extracted from the stored CreateCharacterInfo proto.
    ///
    /// This is the same JSON the lobby logs as "field100.1 customization"
    /// and contains the full FCharacterCustomizationSaveData fields the
    /// AppearanceEmitter needs (presetGuid, skinColorHue, headHair, etc.).
    std::string last_character_customization_json() const {
        std::lock_guard<std::mutex> lk(chars_mutex_);
        const StoredCharacter* sc = nullptr;
        if (!current_play_character_id_.empty()) {
            for (const auto& c : characters_) {
                if (c.character_id == current_play_character_id_) {
                    sc = &c;
                    break;
                }
            }
        }
        if (!sc && !characters_.empty()) sc = &characters_.back();
        if (!sc) return std::string{};
        return extract_customization_json(sc->create_info_raw);
    }

    /// Proxy mode: forward all traffic to real server and log everything
    void set_proxy_channel(std::shared_ptr<grpc::Channel> channel) {
        proxy_channel_ = std::move(channel);
        proxy_stub_ = ics_xclient::XClientService::NewStub(proxy_channel_);
        spdlog::info("[XClient] PROXY MODE enabled");
    }

    bool is_proxy_mode() const { return proxy_stub_ != nullptr; }

    grpc::Status ProcessXClientMessage(
        grpc::ServerContext* ctx,
        grpc::ServerReaderWriter<ics_common::MessageWrapper,
                                 ics_common::MessageWrapper>* stream) override
    {
        spdlog::info("[XClient] Stream opened from {}", ctx->peer());

        // â”€â”€ PROXY MODE â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (proxy_stub_) {
            return proxy_forward(ctx, stream);
        }

        // â”€â”€ EMULATION MODE â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        ics_common::MessageWrapper in;
        while (stream->Read(&in)) {
            const auto& t = in.message_type_name();
            spdlog::info("[XClient] << {} (status={} data={}B)",
                         t, (int)in.status_code(), in.message_data().size());
            log_system_data("<<", in);

            if      (t == "ics_common.SessionRequest")              on_session(in, stream);
            else if (t == "ics_common.KeepAliveMessage")            on_keepalive(in, stream);
            else if (t == "ics_xclient.CheckGameVersionRequest")    on_check_version(in, stream);
            else if (t == "ics_xclient.AuthRefreshRequest")         on_auth_refresh(in, stream);
            else if (t == "ics_xclient.GetWorldsRequest")           on_get_worlds(in, stream);
            else if (t == "ics_xclient.GameClientInterestEvent")    on_interest(in, stream);
            else if (t == "ics_xclient.GetCharactersRequest")       on_get_chars(in, stream);
            else if (t == "ics_xclient.CheckCharacterNameRequest")  on_check_name(in, stream);
            else if (t == "ics_xclient.CreateCharacterRequest")     on_create_char(in, stream);
            else if (t == "ics_xclient.SelectCharacterRequest")     on_select_char(in, stream);
            else if (t == "ics_xclient.CanDeleteCharacterRequest")  on_can_delete(in, stream);
            else if (t == "ics_xclient.DeleteCharacterRequest")     on_delete_char(in, stream);
            else if (t == "ics_xclient.PlayRequest")                on_play(in, stream);
            else if (t == "ics_xclient.PlayEndRequest")             on_play_end(in, stream);
            else if (t == "ics_xclient.GetShopHandoffCodeRequest")  on_shop(in, stream);
            else {
                spdlog::warn("[XClient] !! UNHANDLED '{}' hex:\n{}", t, hex_dump(in.message_data()));
                send(stream, make_response(t, "", &in));
            }
        }
        spdlog::info("[XClient] Stream closed");
        return grpc::Status::OK;
    }

private:
    using Stream = grpc::ServerReaderWriter<ics_common::MessageWrapper,
                                            ics_common::MessageWrapper>;
    std::vector<WorldConfig> worlds_;
    std::string game_addr_ = "127.0.0.1:7777";
    uint32_t session_id_ = 0;
    std::function<bool(const std::string&)> relay_callback_;
    std::vector<StoredCharacter> characters_;  // In-memory character store

    // PM38 (2026-04-30) — character_id of the character the user picked
    // via on_play().  Used by last_character_name() etc. to return the
    // ACTIVE play character (not just the latest created).  Reset to
    // empty when on_play_end() fires so subsequent reads fall back.
    std::string current_play_character_id_;
    mutable std::mutex chars_mutex_;           // guards characters_

    // ── Phase B — Character persistence ──────────────────────────────────
    //
    // Saves `characters_` to data/characters.json whenever the list
    // changes (create / delete).  Loads on startup in the constructor.
    // Path is relative to the server's working directory (dist/Release/),
    // which matches how the launcher scripts are invoked.
    //
    // Binary fields (create_info_raw) are hex-encoded for JSON safety.
    //
    // Caller contract: save_characters() MUST be called with
    // chars_mutex_ already held, OR from a context where no other
    // thread can mutate the vector.  Call sites in on_create_char /
    // on_delete_char do both (the mutations are already mutex-guarded).

    static std::string characters_json_path() {
        // Always under CWD/data/ so `dist/Release/data/characters.json`.
        return "data/characters.json";
    }
// -- C-O-A . Cyb --

    static std::string hex_encode(const std::string& bytes) {
        std::string out;
        out.reserve(bytes.size() * 2);
        static const char hex[] = "0123456789abcdef";
        for (unsigned char b : bytes) {
            out += hex[b >> 4];
            out += hex[b & 0xF];
        }
        return out;
    }

    static std::string hex_decode(const std::string& hex) {
        std::string out;
        if (hex.size() & 1) return out;
        out.reserve(hex.size() / 2);
        auto nyb = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        for (size_t i = 0; i < hex.size(); i += 2) {
            int hi = nyb(hex[i]), lo = nyb(hex[i + 1]);
            if (hi < 0 || lo < 0) { out.clear(); return out; }
            out += static_cast<char>((hi << 4) | lo);
        }
        return out;
    }

    // Persist the current `characters_` vector to JSON.  No-op on I/O
    // failure — we log and continue; losing persistence shouldn't crash
    // the server.  Caller holds chars_mutex_.
    void save_characters_locked() const {
        try {
            std::filesystem::path p = characters_json_path();
            std::filesystem::create_directories(p.parent_path());
            nlohmann::json j = nlohmann::json::array();
            for (const auto& c : characters_) {
                nlohmann::json e;
                e["character_id"]       = c.character_id;
                e["character_name"]     = c.character_name;
                e["world_id"]           = c.world_id;
                e["create_info_hex"]    = hex_encode(c.create_info_raw);
                e["created_at_ms"]      = c.created_at_ms;
                e["archetype_id"]       = c.archetype_id;
                e["race_id"]            = c.race_id;
                e["gender_id"]          = c.gender_id;
                e["level"]              = c.level;
                e["class_name"]         = c.class_name;
                e["race_name"]          = c.race_name;
                e["gender_name"]        = c.gender_name;
                j.push_back(std::move(e));
            }
            std::ofstream f(p);
            if (!f) {
                spdlog::warn("[XClient] Could not open {} for write",
                             p.string());
                return;
            }
            f << j.dump(2);
            spdlog::info("[XClient] Persisted {} character(s) to {}",
                         characters_.size(), p.string());
        } catch (const std::exception& e) {
            spdlog::warn("[XClient] save_characters failed: {}", e.what());
        }
    }

    // Load characters from disk into `characters_` at startup.  Missing
    // file is not an error — just means "no saved characters yet".
    void load_characters() {
        try {
            std::filesystem::path p = characters_json_path();
            if (!std::filesystem::exists(p)) {
                spdlog::info("[XClient] No persisted characters at {} "
                             "(first run)", p.string());
                return;
            }
            std::ifstream f(p);
            if (!f) {
                spdlog::warn("[XClient] Could not open {} for read",
                             p.string());
                return;
            }
            nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions*/false);
            if (!j.is_array()) {
                spdlog::warn("[XClient] {} is not a JSON array — skipped",
                             p.string());
                return;
            }
            std::lock_guard<std::mutex> lk(chars_mutex_);
            characters_.clear();
            for (const auto& e : j) {
                StoredCharacter c;
                c.character_id   = e.value("character_id",   std::string{});
                c.character_name = e.value("character_name", std::string{});
                c.world_id       = e.value("world_id",       std::string{});
                c.create_info_raw =
                    hex_decode(e.value("create_info_hex",    std::string{}));
                c.created_at_ms  = e.value("created_at_ms",  uint64_t{0});
                c.archetype_id   = e.value("archetype_id",   uint64_t{0});
                c.race_id        = e.value("race_id",        uint64_t{0});
                c.gender_id      = e.value("gender_id",      uint64_t{0});
                c.level          = e.value("level",          uint32_t{1});
                c.class_name     = e.value("class_name",     std::string{});
                c.race_name      = e.value("race_name",      std::string{});
                c.gender_name    = e.value("gender_name",    std::string{});
                characters_.push_back(std::move(c));
            }
            spdlog::info("[XClient] Loaded {} character(s) from {}",
                         characters_.size(), p.string());
        } catch (const std::exception& e) {
            spdlog::warn("[XClient] load_characters failed: {}", e.what());
        }
    }

    int count_chars_in_world(const std::string& world_id) const {
        int n = 0;
        for (const auto& c : characters_)
            if (c.world_id == world_id) ++n;
        return n;
    }

    // Proxy mode members
    std::shared_ptr<grpc::Channel> proxy_channel_;
    std::unique_ptr<ics_xclient::XClientService::Stub> proxy_stub_;

    // â”€â”€ Proxy bidirectional forwarding â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    static void log_wrapper_detail(const std::string& direction,
                                    const ics_common::MessageWrapper& w) {
        auto wire = w.SerializeAsString();
        spdlog::info("[PROXY] {} type='{}' status={} inner={}B wire={}B",
                     direction, w.message_type_name(),
                     (int)w.status_code(), w.message_data().size(), wire.size());

        // Log system_data tags
        if (w.has_system_data()) {
            std::ostringstream oss;
            for (const auto& kv : w.system_data().tags())
                oss << kv.first << ":" << kv.second << ", ";
            spdlog::info("[PROXY]    system_data: tags={{{}}}", oss.str());
        } else {
            spdlog::info("[PROXY]    system_data: <absent>");
        }
// C Y B captures, C O A code

        // Hex dump of wrapper wire bytes
        spdlog::info("[PROXY]    WIRE: {}", hex_dump(wire, 32768));

        // Hex dump of inner message_data
        if (!w.message_data().empty()) {
            spdlog::info("[PROXY]    INNER: {}", hex_dump(w.message_data(), 32768));
        }

        // Try to decode known types for human-readable output
        const auto& t = w.message_type_name();
        if (t == "ics_xclient.GetWorldsReply") {
            ics_xclient::GetWorldsReply rep;
            if (rep.ParseFromString(w.message_data())) {
                spdlog::info("[PROXY]    DECODED GetWorldsReply: {} worlds", rep.worlds_size());
                for (int i = 0; i < rep.worlds_size(); ++i) {
                    auto& wi = rep.worlds(i);
                    spdlog::info("[PROXY]      [{}] id='{}' name='{}' avail={} full={} q={} c={}",
                                 i, wi.world_id(), wi.world_display_name(),
                                 wi.available(), wi.fullness(),
                                 wi.queue_job_count_hi(), wi.character_count());
                }
            }
        } else if (t == "ics_xclient.GetCharactersReply") {
            ics_xclient::GetCharactersReply rep;
            if (rep.ParseFromString(w.message_data())) {
                spdlog::info("[PROXY]    DECODED GetCharactersReply: {} characters", rep.characters_raw_size());
                for (int i = 0; i < rep.characters_raw_size(); ++i) {
                    const auto& raw = rep.characters_raw(i);
                    spdlog::info("[PROXY]      â”€â”€ Character [{}] raw={}B â”€â”€", i, raw.size());
                    // Decode every field in the Character proto
                    auto cfields = decode_raw_proto(raw);
                    for (const auto& cf : cfields) {
                        switch (cf.wire_type) {
                        case 0:
                            spdlog::info("[PROXY]        field {:3d} (varint)  = {} (0x{:X})",
                                         cf.field_num, cf.varint_val, cf.varint_val);
                            break;
                        case 1:
                            spdlog::info("[PROXY]        field {:3d} (fixed64) = {} (0x{:X})",
                                         cf.field_num, cf.fixed64_val, cf.fixed64_val);
                            break;
                        case 2: {
                            bool is_text = cf.bytes_val.size() > 0 && cf.bytes_val.size() < 300;
                            if (is_text) {
                                for (unsigned char c : cf.bytes_val) {
                                    if (c < 32 && c != '\n' && c != '\r' && c != '\t') { is_text = false; break; }
                                    if (c > 126) { is_text = false; break; }
                                }
                            }
                            if (is_text)
                                spdlog::info("[PROXY]        field {:3d} (string, {:5d}B) = \"{}\"",
                                             cf.field_num, cf.bytes_val.size(),
                                             cf.bytes_val.size() <= 120 ? cf.bytes_val : cf.bytes_val.substr(0, 117) + "...");
                            else {
                                spdlog::info("[PROXY]        field {:3d} (bytes,  {:5d}B) hex: {}",
                                             cf.field_num, cf.bytes_val.size(), hex_dump(cf.bytes_val, 64));
                                // If it's field 100 (customization), decode sub-fields
                                if (cf.field_num == 100) {
                                    auto subfields = decode_raw_proto(cf.bytes_val);
                                    for (const auto& sf : subfields) {
                                        if (sf.wire_type == 2) {
                                            spdlog::info("[PROXY]          field100.{} (bytes, {}B)", sf.field_num, sf.bytes_val.size());
                                            // Go one more level
                                            auto sub2 = decode_raw_proto(sf.bytes_val);
                                            for (const auto& s2 : sub2) {
                                                if (s2.wire_type == 2) {
                                                    bool txt = s2.bytes_val.size() < 200;
                                                    if (txt) for (unsigned char c : s2.bytes_val) { if (c < 32 || c > 126) { txt = false; break; } }
                                                    if (txt)
                                                        spdlog::info("[PROXY]            .{}.{} (str, {}B) = \"{}\"",
                                                                     sf.field_num, s2.field_num, s2.bytes_val.size(),
                                                                     s2.bytes_val.size() <= 120 ? s2.bytes_val : s2.bytes_val.substr(0, 117) + "...");
                                                    else
                                                        spdlog::info("[PROXY]            .{}.{} (bytes, {}B)", sf.field_num, s2.field_num, s2.bytes_val.size());
                                                } else if (s2.wire_type == 0) {
                                                    spdlog::info("[PROXY]            .{}.{} (varint) = {}", sf.field_num, s2.field_num, s2.varint_val);
                                                }
                                            }
                                        } else if (sf.wire_type == 0) {
                                            spdlog::info("[PROXY]          field100.{} (varint) = {}", sf.field_num, sf.varint_val);
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case 5:
                            spdlog::info("[PROXY]        field {:3d} (fixed32) = {} (0x{:X})",
                                         cf.field_num, cf.fixed32_val, cf.fixed32_val);
                            break;
                        }
                    }
                    // Also extract and summarize customization JSON
                    std::string json = extract_customization_json(raw);
                    if (!json.empty()) {
                        spdlog::info("[PROXY]        customization JSON: {}B", json.size());
                        // Extract key fields from JSON
                        std::string cls = extract_json_string(json, "class");
                        std::string race = extract_json_string(json, "race");
                        std::string gender = extract_json_string(json, "gender");
                        if (!cls.empty() || !race.empty() || !gender.empty())
                            spdlog::info("[PROXY]        JSON: class='{}' race='{}' gender='{}'", cls, race, gender);
                    }
                }
            }
        } else if (t == "ics_xclient.WorldStatusEvent") {
            ics_xclient::WorldStatusEvent evt;
            if (evt.ParseFromString(w.message_data())) {
                spdlog::info("[PROXY]    DECODED WorldStatusEvent: {} worlds", evt.worlds_size());
                for (int i = 0; i < evt.worlds_size(); ++i) {
                    auto& wi = evt.worlds(i);
                    spdlog::info("[PROXY]      [{}] id='{}' name='{}' avail={} full={} q={} c={}",
                                 i, wi.world_id(), wi.world_display_name(),
                                 wi.available(), wi.fullness(),
                                 wi.queue_job_count_hi(), wi.character_count());
                }
            }
        } else if (t == "ics_xclient.CreateCharacterRequest") {
            ics_xclient::CreateCharacterRequest creq;
            if (creq.ParseFromString(w.message_data())) {
                spdlog::info("[PROXY]    DECODED CreateCharacterRequest: name='{}' world='{}' info_raw={}B",
                             creq.character_name(), creq.world_id(), creq.create_character_info_raw().size());
                if (!creq.create_character_info_raw().empty()) {
                    auto cfields = decode_raw_proto(creq.create_character_info_raw());
                    spdlog::info("[PROXY]      CreateCharacterInfo fields ({}):", cfields.size());
                    for (const auto& cf : cfields) {
                        if (cf.wire_type == 0) spdlog::info("[PROXY]        field {:3d} (varint) = {} (0x{:X})", cf.field_num, cf.varint_val, cf.varint_val);
                        else if (cf.wire_type == 2) {
                            bool txt = cf.bytes_val.size() < 200;
                            if (txt) for (unsigned char c : cf.bytes_val) { if (c < 32 || c > 126) { txt = false; break; } }
                            if (txt) spdlog::info("[PROXY]        field {:3d} (string, {}B) = \"{}\"", cf.field_num, cf.bytes_val.size(),
                                                  cf.bytes_val.size() <= 120 ? cf.bytes_val : cf.bytes_val.substr(0, 117) + "...");
                            else spdlog::info("[PROXY]        field {:3d} (bytes, {}B)", cf.field_num, cf.bytes_val.size());
                        }
                    }
                    std::string json = extract_customization_json(creq.create_character_info_raw());
                    if (!json.empty()) {
                        std::string cls = extract_json_string(json, "class");
                        std::string race = extract_json_string(json, "race");
                        std::string gender = extract_json_string(json, "gender");
                        spdlog::info("[PROXY]      JSON: class='{}' race='{}' gender='{}'", cls, race, gender);
                    }
                }
            }
        } else if (t == "ics_xclient.CreateCharacterReply") {
            ics_xclient::CreateCharacterReply crep;
            if (crep.ParseFromString(w.message_data())) {
                spdlog::info("[PROXY]    DECODED CreateCharacterReply: id='{}' name='{}' world='{}' info_raw={}B",
                             crep.character_id(), crep.character_name(), crep.world_id(), crep.character_info_raw().size());
                if (!crep.character_info_raw().empty()) {
                    auto cfields = decode_raw_proto(crep.character_info_raw());
                    spdlog::info("[PROXY]      character_info_raw fields ({}):", cfields.size());
                    for (const auto& cf : cfields) {
                        if (cf.wire_type == 0) spdlog::info("[PROXY]        field {:3d} (varint) = {} (0x{:X})", cf.field_num, cf.varint_val, cf.varint_val);
                        else if (cf.wire_type == 2) {
                            bool txt = cf.bytes_val.size() < 300;
                            if (txt) for (unsigned char c : cf.bytes_val) { if (c < 32 || c > 126) { txt = false; break; } }
                            if (txt) spdlog::info("[PROXY]        field {:3d} (string, {}B) = \"{}\"", cf.field_num, cf.bytes_val.size(),
                                                  cf.bytes_val.size() <= 120 ? cf.bytes_val : cf.bytes_val.substr(0, 117) + "...");
                            else spdlog::info("[PROXY]        field {:3d} (bytes, {}B) hex: {}", cf.field_num, cf.bytes_val.size(), hex_dump(cf.bytes_val, 64));
                        }
                    }
                }
            }
        } else if (t == "ics_xclient.SelectCharacterRequest") {
            ics_xclient::SelectCharacterRequest sreq;
            if (sreq.ParseFromString(w.message_data()))
                spdlog::info("[PROXY]    DECODED SelectCharacterRequest: char_id='{}'", sreq.character_id());
        } else if (t == "ics_xclient.SelectCharacterReply") {
            ics_xclient::SelectCharacterReply srep;
            if (srep.ParseFromString(w.message_data()))
                spdlog::info("[PROXY]    DECODED SelectCharacterReply: char_id='{}'", srep.character_id());
        } else if (t == "ics_xclient.PlayRequest") {
            ics_xclient::PlayRequest preq;
            if (preq.ParseFromString(w.message_data()))
                spdlog::info("[PROXY]    DECODED PlayRequest: char_id='{}' world='{}'", preq.character_id(), preq.world_id());
        } else if (t == "ics_xclient.PlayReply") {
            ics_xclient::PlayReply prep;
            if (prep.ParseFromString(w.message_data()))
                spdlog::info("[PROXY]    DECODED PlayReply: token='{}' conn='{}'",
                             prep.game_connection_token().substr(0, 40) + "...", prep.game_connection_string());
        } else if (t == "ics_xclient.PlayerSessionState") {
            ics_xclient::PlayerSessionState st;
            if (st.ParseFromString(w.message_data())) {
                spdlog::info("[PROXY]    DECODED PlayerSessionState: char='{}' world='{}' pending={} mini_records={}",
                             st.character_id(), st.world_id(),
                             st.has_pending_play_request(), st.character_mini_records_size());
                for (int i = 0; i < st.character_mini_records_size(); ++i) {
                    auto& r = st.character_mini_records(i);
                    spdlog::info("[PROXY]      mini_record[{}]: id='{}' name='{}' world='{}'",
                                 i, r.character_id(), r.character_name(), r.world_id());
                }
            }
        } else if (t == "ics_common.SessionReply") {
            ics_common::SessionReply rep;
            if (rep.ParseFromString(w.message_data())) {
                spdlog::info("[PROXY]    DECODED SessionReply: result={} type={}",
                             (int)rep.result_code(), (int)rep.request_type());
            }
        } else if (t == "ics_xclient.CheckGameVersionReply") {
            ics_xclient::CheckGameVersionReply rep;
            if (rep.ParseFromString(w.message_data())) {
                spdlog::info("[PROXY]    DECODED CheckGameVersionReply: supported={} msg='{}'",
                             rep.supported(), rep.message());
            }
        } else if (t == "ics_xclient.GameClientInterestEvent") {
            ics_xclient::GameClientInterestEvent evt;
            if (evt.ParseFromString(w.message_data())) {
                spdlog::info("[PROXY]    DECODED InterestEvent: start=0x{:X} stop=0x{:X}",
                             evt.start_interest_bitmask(), evt.stop_interest_bitmask());
            }
        }
    }
// [CoA x C | Y | B]

    grpc::Status proxy_forward(
        grpc::ServerContext* server_ctx,
        Stream* client_stream)
    {
        spdlog::info("[PROXY] ======== PROXY SESSION START ========");
        ProxyLogger::instance().log_event("xclient", "session_start");

        // Open bidi stream to real server
        grpc::ClientContext upstream_ctx;

        // Forward client metadata (skip reserved/transport headers)
        for (const auto& md : server_ctx->client_metadata()) {
            std::string key(md.first.data(), md.first.size());
            std::string val(md.second.data(), md.second.size());
            // Skip reserved gRPC headers, pseudo-headers, and transport headers
            // user-agent is set on the channel via GRPC_ARG_PRIMARY_USER_AGENT_STRING
            if (key.find("grpc-") == 0 || (!key.empty() && key[0] == ':') ||
                key == "user-agent" || key == "te" || key == "content-type") continue;
            upstream_ctx.AddMetadata(key, val);
            spdlog::info("[PROXY] Forwarding metadata: {}={}", key, val);
        }

        auto upstream_stream = proxy_stub_->ProcessXClientMessage(&upstream_ctx);
        if (!upstream_stream) {
            spdlog::error("[PROXY] Failed to open upstream stream!");
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                               "proxy: cannot connect to upstream");
        }
        spdlog::info("[PROXY] Upstream stream opened to real server");

        std::atomic<bool> client_done{false};
        std::atomic<bool> server_done{false};
        grpc::Status upstream_status;

        // Thread: read from REAL SERVER â†’ log â†’ (rewrite if needed) â†’ write to CLIENT
        std::thread server_reader([&]() {
            ics_common::MessageWrapper msg;
            int count = 0;
            while (upstream_stream->Read(&msg)) {
                ++count;
                spdlog::info("[PROXY] â”€â”€â”€â”€ SERVER >> CLIENT #{} â”€â”€â”€â”€", count);
                log_wrapper_detail("SERVER>>", msg);
                ProxyLogger::instance().log("xclient", "S>C", count, msg);
// [C-O-A :: C Y B]

                // â”€â”€ Intercept PlayReply to rewrite connection address â”€â”€â”€â”€
                // The real server returns something like:
                //   game_connection_string = "ec2-18-223-41-103.us-east-2.compute.amazonaws.com:7229"
                // We need to replace it with our local address so the client
                // connects through our UDP relay proxy.
                if (msg.message_type_name() == "ics_xclient.PlayReply") {
                    ics_xclient::PlayReply prep;
                    if (prep.ParseFromString(msg.message_data()) &&
                        !prep.game_connection_string().empty()) {
                        std::string original = prep.game_connection_string();
                        std::string rewritten = game_addr_;
                        prep.set_game_connection_string(rewritten);
                        msg.set_message_data(prep.SerializeAsString());
                        spdlog::warn("[PROXY] *** PlayReply REWRITE: '{}' â†’ '{}'",
                                     original, rewritten);
                        ProxyLogger::instance().log_event("xclient", "playreply_rewrite", {
                            {"original", original},
                            {"rewritten", rewritten},
                            {"token_prefix", prep.game_connection_token().substr(0,
                                std::min<size_t>(16, prep.game_connection_token().size()))}
                        });
                        // Dynamically activate UDP relay to the real game server
                        if (relay_callback_ && !original.empty()) {
                            spdlog::warn("[PROXY] Activating UDP relay to '{}'", original);
                            if (!relay_callback_(original)) {
                                spdlog::error("[PROXY] Failed to activate UDP relay!");
                            }
                        }
                    }
                }
                if (!client_stream->Write(msg)) {
                    spdlog::error("[PROXY] Write to client FAILED at msg #{}", count);
                    break;
                }
            }
            server_done = true;
            spdlog::info("[PROXY] Server reader done ({} messages)", count);
        });

        // Main thread: read from CLIENT â†’ log â†’ write to REAL SERVER
        {
            ics_common::MessageWrapper msg;
            int count = 0;
            while (client_stream->Read(&msg)) {
                ++count;
                spdlog::info("[PROXY] â”€â”€â”€â”€ CLIENT >> SERVER #{} â”€â”€â”€â”€", count);
                log_wrapper_detail("CLIENT>>", msg);
                ProxyLogger::instance().log("xclient", "C>S", count, msg);
                if (!upstream_stream->Write(msg)) {
                    spdlog::error("[PROXY] Write to upstream FAILED at msg #{}", count);
                    break;
                }
            }
            client_done = true;
            spdlog::info("[PROXY] Client reader done ({} messages)", count);
            upstream_stream->WritesDone();
        }

        // Wait for server reader thread
        server_reader.join();
        upstream_status = upstream_stream->Finish();

        spdlog::info("[PROXY] ======== PROXY SESSION END ========");
        spdlog::info("[PROXY] Upstream status: {} ({})",
                     (int)upstream_status.error_code(),
                     upstream_status.error_message());
        ProxyLogger::instance().log_event("xclient", "session_end", {
            {"status_code", (int)upstream_status.error_code()},
            {"status_msg", upstream_status.error_message()}
        });

        return grpc::Status::OK;
    }

    static void fill(ics_xclient::WorldInfo* wi, const WorldConfig& c) {
        wi->set_world_id(c.world_id);
        wi->set_world_display_name(c.world_display_name);
        wi->set_available(c.available);
        wi->set_fullness(c.fullness);
        wi->set_queue_job_count_hi(c.queue_job_count_hi);
        wi->set_character_count(c.character_count);
    }

    bool send(Stream* s, ics_common::MessageWrapper m,
              const std::string& ftag_val = "", bool dump = false) {
        // Enrich system_data with server-side tags (like real Intrepid server)
        auto* tags = m.mutable_system_data()->mutable_tags();
        if (!ftag_val.empty())
            (*tags)["ftag"] = ftag_val;
        (*tags)["server_id"] = "aoc-emu-local";
        (*tags)["msg_uuid"] = generate_msg_uuid();
        (*tags)["server_session_id"] = std::to_string(session_id_);
        (*tags)["server_epoch_nanosec"] = std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        if (dump) {
            auto wire = m.SerializeAsString();
            spdlog::info("[XClient] >> WIRE {} bytes type='{}'\n  {}",
                         wire.size(), m.message_type_name(), hex_dump(wire));
            spdlog::info("[XClient] >> INNER {} bytes\n  {}",
                         m.message_data().size(), hex_dump(m.message_data()));
        }
        bool ok = s->Write(m);
        if (!ok) spdlog::error("[XClient] !! Write FAILED '{}'", m.message_type_name());
        return ok;
    }

    // â”€â”€ handlers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    void on_session(const ics_common::MessageWrapper& in, Stream* s) {
        ics_common::SessionRequest req;
        if (!req.ParseFromString(in.message_data())) {
            spdlog::error("[XClient] parse SessionRequest FAILED"); return;
        }
        spdlog::info("[XClient] SessionRequest type={}", (int)req.request_type());
        for (auto& kv : req.tags())     spdlog::info("[XClient]   tag  {}={}", kv.first, kv.second);
        for (auto& kv : req.data_map()) spdlog::info("[XClient]   data {}={}", kv.first, kv.second);
        if (!req.data_string().empty()) spdlog::info("[XClient]   data_string='{}'", req.data_string());

        // Assign session ID on open
        if (req.request_type() == ics_common::SESSION_REQUEST_TYPE_OPEN) {
            static std::atomic<uint32_t> next_session{10000};
            session_id_ = next_session.fetch_add(1);
        }

        // Build SessionReply matching real server structure
        ics_common::SessionReply rep;
        rep.set_result_code(ics_common::ICS_STATUS_CODE_SUCCESS);
        rep.set_request_type(req.request_type());

        if (req.request_type() == ics_common::SESSION_REQUEST_TYPE_OPEN) {
            // data_map entry 1: ping client configuration
            // (real server sends this to configure client ping/latency system)
            // Real server sends actual ping hosts for latency measurement
            (*rep.mutable_data_map())["ics_ping_client_config.json"] =
                R"({"log_level":"info","ping_hosts":[{"host":"ping-eunorth1.ashesofcreation.com","name":"eunorth1"},{"host":"ping-useast2.ashesofcreation.com","name":"useast2"}],"ping_period_msec":2000,"version":"0.2.13"})";

            // data_map entry 2: player session state
            // (real server embeds the serialized PlayerSessionState here)
            ics_xclient::PlayerSessionState state;
            // Populate mini records from stored characters
            for (const auto& c : characters_) {
                auto* rec = state.add_character_mini_records();
                rec->set_character_id(c.character_id);
                rec->set_character_name(c.character_name);
                rec->set_world_id(c.world_id);
            }
            (*rep.mutable_data_map())["player_session_state"] =
                state.SerializeAsString();

            spdlog::info("[XClient] SessionReply data_map: ping_config + player_session_state ({}B)",
                         state.ByteSizeLong());
        }

        send(s, make_response("ics_common.SessionReply", rep.SerializeAsString(), &in),
             "ics_session", true);
        spdlog::info("[XClient] >> SessionReply OK (data_map={} entries, session_id={})",
                     rep.data_map_size(), session_id_);
    }

    void on_keepalive(const ics_common::MessageWrapper& in, Stream* s) {
        ics_common::KeepAliveMessage ka;
        ka.set_timestamp_msec(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        send(s, make_response("ics_common.KeepAliveMessage", ka.SerializeAsString(), &in),
             "ics_keep_alive");
    }
// ~ C-O-A & C_Y_B ~

    void on_check_version(const ics_common::MessageWrapper& in, Stream* s) {
        ics_xclient::CheckGameVersionRequest req;
        req.ParseFromString(in.message_data());
        spdlog::info("[XClient] CheckGameVersion '{}'", req.game_version());
        ics_xclient::CheckGameVersionReply rep;
        rep.set_supported(true);
        // Leave message empty - non-empty might trigger client popup
        send(s, make_response("ics_xclient.CheckGameVersionReply", rep.SerializeAsString(), &in),
             "ics_lobby", true);
        spdlog::info("[XClient] >> CheckGameVersionReply supported=true (with system_data)");
    }

    void on_auth_refresh(const ics_common::MessageWrapper& in, Stream* s) {
        spdlog::info("[XClient] AuthRefresh");
        ics_xclient::AuthRefreshReply rep;
        rep.set_auth_token("emu-auth-refreshed");
        rep.set_refresh_token("emu-refresh-refreshed");
        send(s, make_response("ics_xclient.AuthRefreshReply", rep.SerializeAsString(), &in));
    }

    void on_get_worlds(const ics_common::MessageWrapper& in, Stream* s) {
        spdlog::info("[XClient] GetWorlds ({} configured)", worlds_.size());
        ics_xclient::GetWorldsReply rep;
        for (auto& w : worlds_) fill(rep.add_worlds(), w);
        auto inner = rep.SerializeAsString();

        for (int i = 0; i < rep.worlds_size(); ++i) {
            auto& wi = rep.worlds(i);
            spdlog::info("[XClient]   [{}] id='{}' name='{}' avail={} full={} q={} c={}",
                         i, wi.world_id(), wi.world_display_name(),
                         wi.available(), wi.fullness(),
                         wi.queue_job_count_hi(), wi.character_count());
        }

        send(s, make_response("ics_xclient.GetWorldsReply", inner, &in),
             "ics_lobby", true);
        spdlog::info("[XClient] >> GetWorldsReply OK ({} realms, {} inner bytes, with system_data)",
                     rep.worlds_size(), inner.size());
    }

    void on_interest(const ics_common::MessageWrapper& in, Stream* s) {
        ics_xclient::GameClientInterestEvent evt;
        evt.ParseFromString(in.message_data());
        spdlog::info("[XClient] Interest start=0x{:X} stop=0x{:X}",
                     evt.start_interest_bitmask(), evt.stop_interest_bitmask());

        // WorldStatus subscription - push current world status
        // Bit 1 = WorldStatus (enum value 1), but also try bit 0 just in case
        if (evt.start_interest_bitmask() != 0) {
            ics_xclient::WorldStatusEvent wse;
            for (auto& w : worlds_) fill(wse.add_worlds(), w);
            send(s, make_push("ics_xclient.WorldStatusEvent", wse.SerializeAsString()),
                 "ics_lobby", true);
            spdlog::info("[XClient] >> WorldStatusEvent push ({} realms)", wse.worlds_size());
        }
    }

    void on_get_chars(const ics_common::MessageWrapper& in, Stream* s) {
        ics_xclient::GetCharactersRequest req; req.ParseFromString(in.message_data());
        spdlog::info("[XClient] GetCharacters world='{}'", req.world_id());
        ics_xclient::GetCharactersReply rep;
        for (const auto& c : characters_) {
            if (c.world_id == req.world_id()) {
                std::string char_proto = build_character_proto(c);
                rep.add_characters_raw(char_proto);
                spdlog::info("[XClient]   char '{}' id={} proto={}B",
                             c.character_name, c.character_id, char_proto.size());
                spdlog::debug("[XClient]   char proto hex:\n{}",
                              hex_dump(char_proto, 256));
            }
        }
        spdlog::info("[XClient] >> GetCharactersReply {} chars for world '{}'",
                     rep.characters_raw_size(), req.world_id());
        send(s, make_response("ics_xclient.GetCharactersReply", rep.SerializeAsString(), &in),
             "", true);
    }

    void on_check_name(const ics_common::MessageWrapper& in, Stream* s) {
        ics_xclient::CheckCharacterNameRequest req; req.ParseFromString(in.message_data());
        spdlog::info("[XClient] CheckName '{}' world='{}'", req.character_name(), req.world_id());
        ics_xclient::CheckCharacterNameReply rep;
        rep.set_character_name(req.character_name()); rep.set_available(true);
        send(s, make_response("ics_xclient.CheckCharacterNameReply", rep.SerializeAsString(), &in));
    }

    void on_create_char(const ics_common::MessageWrapper& in, Stream* s) {
        ics_xclient::CreateCharacterRequest req; req.ParseFromString(in.message_data());
        spdlog::info("[XClient] CreateChar '{}' world='{}' info_raw={}B",
                     req.character_name(), req.world_id(),
                     req.create_character_info_raw().size());

        // Log raw creation data for future proto analysis
        if (!req.create_character_info_raw().empty()) {
            spdlog::info("[XClient] create_character_info_raw:\n{}",
                         hex_dump(req.create_character_info_raw(), 4096));
        }

        // Generate 32-char hex character ID (matching real server format)
        std::string char_id = generate_msg_uuid();

        // Store character in memory
        StoredCharacter sc;
        sc.character_id    = char_id;
        sc.character_name  = req.character_name();
        sc.world_id        = req.world_id();
        sc.create_info_raw = req.create_character_info_raw();
        sc.created_at_ms   = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        sc.level = 1;  // New characters start at level 1

        // Extract archetype/race/gender from CreateCharacterInfo
        if (!req.create_character_info_raw().empty()) {
            extract_create_info(req.create_character_info_raw(), sc);
        }

        {
            std::lock_guard<std::mutex> lk(chars_mutex_);
            characters_.push_back(std::move(sc));
            save_characters_locked();  // Phase B: persist to disk
        }

        // Update world character count
        for (auto& w : worlds_) {
            if (w.world_id == req.world_id())
                w.character_count = count_chars_in_world(req.world_id());
        }

        spdlog::info("[XClient] Stored char '{}' id={} (total: {})",
                     req.character_name(), char_id, characters_.size());

        ics_xclient::CreateCharacterReply rep;
        rep.set_character_id(char_id);
        rep.set_character_name(req.character_name());
        rep.set_world_id(req.world_id());
        // Build the CreateCharacterReply character_info_raw.
        // IMPORTANT: This uses a DIFFERENT field layout than GetCharactersReply!
        // CreateCharacterReply: field1=customization, field2=gender, field6=race, field9=1, field14=2
        // GetCharactersReply field100: field1=customization, field2=level, field5=archetype, field6=race, etc.
        {
            const auto& stored = characters_.back();
            std::string reply_info = build_create_reply_info(stored);
            rep.set_character_info_raw(reply_info);
            spdlog::info("[XClient] CreateCharacterReply character_info_raw={}B (CreateReply format)",
                         reply_info.size());
            spdlog::debug("[XClient] CreateCharacterReply info hex:\n{}",
                          hex_dump(reply_info, 512));
        }
        send(s, make_response("ics_xclient.CreateCharacterReply", rep.SerializeAsString(), &in));
    }

    void on_select_char(const ics_common::MessageWrapper& in, Stream* s) {
        ics_xclient::SelectCharacterRequest req; req.ParseFromString(in.message_data());
        spdlog::info("[XClient] SelectChar '{}'", req.character_id());
        ics_xclient::SelectCharacterReply rep; rep.set_character_id(req.character_id());
        send(s, make_response("ics_xclient.SelectCharacterReply", rep.SerializeAsString(), &in));
    }
// built with CoA and C_Y_B

    void on_can_delete(const ics_common::MessageWrapper& in, Stream* s) {
        ics_xclient::CanDeleteCharacterRequest req; req.ParseFromString(in.message_data());
        ics_xclient::CanDeleteCharacterReply rep;
        rep.set_character_id(req.character_id()); rep.set_can_delete(true);
        send(s, make_response("ics_xclient.CanDeleteCharacterReply", rep.SerializeAsString(), &in));
    }

    void on_delete_char(const ics_common::MessageWrapper& in, Stream* s) {
        ics_xclient::DeleteCharacterRequest req; req.ParseFromString(in.message_data());
        spdlog::info("[XClient] DeleteChar '{}' name='{}' world='{}'",
                     req.character_id(), req.character_name(), req.world_id());

        // Remove from store
        {
            std::lock_guard<std::mutex> lk(chars_mutex_);
            auto it = std::remove_if(characters_.begin(), characters_.end(),
                [&](const StoredCharacter& c) { return c.character_id == req.character_id(); });
            if (it != characters_.end()) {
                spdlog::info("[XClient] Removed character '{}'", req.character_id());
                characters_.erase(it, characters_.end());
                save_characters_locked();  // Phase B: persist to disk
            }
        }

        // Update world character count
        for (auto& w : worlds_) {
            if (w.world_id == req.world_id())
                w.character_count = count_chars_in_world(req.world_id());
        }

        ics_xclient::DeleteCharacterReply rep; rep.set_character_id(req.character_id());
        send(s, make_response("ics_xclient.DeleteCharacterReply", rep.SerializeAsString(), &in));
    }

    void on_play(const ics_common::MessageWrapper& in, Stream* s) {
        ics_xclient::PlayRequest req; req.ParseFromString(in.message_data());
        spdlog::info("[XClient] Play char='{}' world='{}'", req.character_id(), req.world_id());

        // PM38 (2026-04-30) — record which character is going into the
        // game session.  last_character_name() and friends now resolve
        // to THIS character (rather than the most-recently-created one).
        // Look up the character to also log a friendly name for the op.
        {
            std::lock_guard<std::mutex> lk(chars_mutex_);
            current_play_character_id_ = req.character_id();
            for (const auto& c : characters_) {
                if (c.character_id == req.character_id()) {
                    spdlog::info("[XClient] >> on_play: selecting '{}' (archetype={} race={} gender={})",
                                  c.character_name, c.archetype_id, c.race_name, c.gender_name);
                    break;
                }
            }
        }

        // Generate token early so it's available for all phases
        auto token = generate_msg_uuid();

        // Phase 1: Send PlayReply with status=1 (ACKNOWLEDGED / in-progress)
        // Real server sends this ~59ms after PlayRequest with empty inner data.
        // The status_code in the wrapper = 1 (ICS_STATUS_CODE_ACKNOWLEDGED)
        {
            auto pending = make_response("ics_xclient.PlayReply", "", &in,
                                          ics_common::ICS_STATUS_CODE_ACKNOWLEDGED);
            send(s, pending, "ics_play_character");
            spdlog::info("[XClient] >> PlayReply phase 1 (status=ACKNOWLEDGED, in-progress)");
        }
// [Cyb + C O A]

        // Phase 2: Send PlayRequestStatusReport (queue position = 0 = ready)
        // Real server sends ~437ms after PlayRequest (~378ms after phase 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        {
            ics_xclient::PlayRequestStatusReport report;
            report.set_queue_position(0);
            send(s, make_push("ics_xclient.PlayRequestStatusReport",
                              report.SerializeAsString()), "ics_play_character");
            spdlog::info("[XClient] >> PlayRequestStatusReport (queue_position=0)");
        }

        // Phase 3: Send final PlayReply with connection info (status=SUCCESS)
        // Real server sends ~852ms after PlayRequest (~415ms after phase 2)
        // Inner protobuf: field1(string)=token, field2(string)=connection
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        {
            ics_xclient::PlayReply rep;
            rep.set_game_connection_token(token);
            rep.set_game_connection_string(game_addr_);
            send(s, make_response("ics_xclient.PlayReply", rep.SerializeAsString(), &in),
                 "ics_play_character");
            spdlog::info("[XClient] >> PlayReply phase 3 (token='{}' conn='{}')", token, game_addr_);
        }
    }

    void on_play_end(const ics_common::MessageWrapper& in, Stream* s) {
        spdlog::info("[XClient] PlayEnd");
        ics_xclient::PlayEndReply rep;
        send(s, make_response("ics_xclient.PlayEndReply", rep.SerializeAsString(), &in));
    }

    void on_shop(const ics_common::MessageWrapper& in, Stream* s) {
        spdlog::info("[XClient] GetShopHandoffCode");
        ics_xclient::GetShopHandoffCodeReply rep;
        rep.set_shop_handoff_code("emu-shop-code-000");
        send(s, make_response("ics_xclient.GetShopHandoffCodeReply", rep.SerializeAsString(), &in));
    }
};

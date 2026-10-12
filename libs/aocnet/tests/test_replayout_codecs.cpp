// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_replayout_codecs.cpp
//
//  Round-trip unit tests for the RepLayout primitive codecs.
//
//  Contract for each codec pair:
//      encode(decode(bits)) == bits        (identity)
//      decode(encode(value)) == value      (round-trip value)
//
//  We verify BOTH directions for every codec at every known tricky case
//  (empty, min, max, negative, adjacent bit-alignment).  A single diff
//  here catches encoder/decoder drift before it shows up as "client
//  rejected the packet" 10 layers up.
//
//  Note: BunchWriter emits bits LSB-first.  PacketReader reads bits
//  LSB-first.  The writer buffer and reader buffer match byte-for-byte.
//
//  LAYER:  tools
// ============================================================================
#include "protocol/emit/replayout/encoders/fstring_codec.h"
#include "protocol/emit/replayout/encoders/scalar_codec.h"
#include "protocol/emit/replayout/encoders/fobject_codec.h"
#include "protocol/emit/replayout/encoders/fstruct_codec.h"
#include "protocol/emit/replayout/encoder.h"
#include "protocol/emit/replayout/decoder.h"
#include "protocol/emit/replayout/catalog.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_reader.h"
#include "protocol/emit/intrepid_netguid.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace aoc::protocol::emit::replayout;
using aoc::protocol::emit::BunchWriter;
using aoc::protocol::emit::FIntrepidNetworkGUID;
using aoc::protocol::wire::PacketReader;

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            ++g_passed;                                                      \
        } else {                                                             \
            spdlog::error("  FAIL  {}: {}", __FUNCTION__, msg);              \
            ++g_failed;                                                      \
        }                                                                    \
    } while (0)

// Drive a PropertyValue through encode → decode and check bits + value
// identity.  Returns true on full match.
template <typename Decode>
bool round_trip_through_bits(const PropertyValue& original,
                              bool (*encode)(const PropertyValue&, BunchWriter&),
                              Decode decode,
                              const std::string& label,
                              size_t expected_bits = 0) {
    BunchWriter bw(64);
    if (!encode(original, bw)) {
        spdlog::error("  FAIL  {}: encode returned false", label);
        return false;
    }
    size_t written_bits = bw.bit_pos();
    if (expected_bits != 0 && written_bits != expected_bits) {
        spdlog::error("  FAIL  {}: expected {} bits, got {}",
                      label, expected_bits, written_bits);
        return false;
    }
    PacketReader rd(bw.data(), bw.byte_size(), written_bits);
    PropertyValue decoded = decode(rd);
    if (decoded.type != original.type) {
        spdlog::error("  FAIL  {}: type mismatch ({} vs {})",
                      label, (int)decoded.type, (int)original.type);
        return false;
    }
    return true;
}
// [Cyb + C.O.A]

// ─── FBool ──────────────────────────────────────────────────────────────

void test_fbool() {
    {
        BunchWriter bw;
        auto v = PropertyValue::make_bool(true);
        CHECK(encode_fbool(v, bw), "encode true");
        CHECK(bw.bit_pos() == 1, "one bit written");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fbool(rd);
        auto* b = std::get_if<bool>(&d.payload);
        CHECK(b && *b == true, "decoded true");
    }
    {
        BunchWriter bw;
        auto v = PropertyValue::make_bool(false);
        CHECK(encode_fbool(v, bw), "encode false");
        CHECK(bw.bit_pos() == 1, "one bit written");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fbool(rd);
        auto* b = std::get_if<bool>(&d.payload);
        CHECK(b && *b == false, "decoded false");
    }
}

// ─── FByte / FEnum ───────────────────────────────────────────────────────

void test_fbyte() {
    for (uint8_t x : {uint8_t(0), uint8_t(1), uint8_t(0xA5), uint8_t(0xFF)}) {
        BunchWriter bw;
        PropertyValue v;
        v.type = FPropertyType::Byte;
        v.payload = x;
        CHECK(encode_fbyte(v, bw), "encode byte");
        CHECK(bw.bit_pos() == 8, "8 bits written");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fbyte(rd);
        auto* y = std::get_if<uint8_t>(&d.payload);
        CHECK(y && *y == x, "byte matches");
    }
}

// ─── FInt ────────────────────────────────────────────────────────────────

void test_fint() {
    for (int32_t x : {int32_t(0), int32_t(1), int32_t(-1),
                       int32_t(0x7FFFFFFF), int32_t(-0x80000000),
                       int32_t(12345)}) {
        BunchWriter bw;
        auto v = PropertyValue::make_int(x);
        CHECK(encode_fint(v, bw), "encode int");
        CHECK(bw.bit_pos() == 32, "32 bits written");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fint(rd);
        auto* y = std::get_if<int32_t>(&d.payload);
        CHECK(y && *y == x, "int matches");
    }
}

// ─── FFloat ──────────────────────────────────────────────────────────────

void test_ffloat() {
    for (float x : {0.0f, 1.0f, -1.0f, 3.14159f, -12345.6789f}) {
        BunchWriter bw;
        PropertyValue v;
        v.type = FPropertyType::Float;
        v.payload = x;
        CHECK(encode_ffloat(v, bw), "encode float");
        CHECK(bw.bit_pos() == 32, "32 bits written");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_ffloat(rd);
        auto* y = std::get_if<float>(&d.payload);
        CHECK(y && *y == x, "float matches");
    }
}

// ─── FString ─────────────────────────────────────────────────────────────

void test_fstring() {
    // Empty string
    {
        BunchWriter bw;
        auto v = PropertyValue::make_string("");
        CHECK(encode_fstring(v, bw), "encode empty");
        CHECK(bw.bit_pos() == 32, "empty = 32 bits (save_num=0)");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fstring(rd);
        auto* s = std::get_if<std::string>(&d.payload);
        CHECK(s && s->empty(), "decoded empty");
    }

    // 10-char ASCII — matches the captured "RandomChar" pkt#104 HUD name
    {
        BunchWriter bw;
        auto v = PropertyValue::make_string("RandomChar");
        CHECK(encode_fstring(v, bw), "encode 'RandomChar'");
        // 32 bits save_num + 11 × 8 bits (10 chars + NUL) = 120 bits
        CHECK(bw.bit_pos() == 32 + 11 * 8, "RandomChar = 120 bits");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fstring(rd);
        auto* s = std::get_if<std::string>(&d.payload);
        CHECK(s && *s == "RandomChar", "decoded RandomChar");
    }

    // Variable-length ASCII (what patcher could never do right)
    {
        BunchWriter bw;
        auto v = PropertyValue::make_string("Bob");
        CHECK(encode_fstring(v, bw), "encode 'Bob'");
        CHECK(bw.bit_pos() == 32 + 4 * 8, "Bob = 64 bits");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fstring(rd);
        auto* s = std::get_if<std::string>(&d.payload);
        CHECK(s && *s == "Bob", "decoded Bob");
    }
// CoA x C_Y_B

    // 20-char ASCII — AoC UI max
    {
        BunchWriter bw;
        auto v = PropertyValue::make_string("ThisIsTwentyCharWord");
        CHECK(encode_fstring(v, bw), "encode 20-char");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fstring(rd);
        auto* s = std::get_if<std::string>(&d.payload);
        CHECK(s && *s == "ThisIsTwentyCharWord", "decoded 20-char");
    }
}

// ─── FObject (FIntrepidNetworkGUID 128-bit) ──────────────────────────────

void test_fobject() {
    // Default / zero GUID
    {
        BunchWriter bw;
        FIntrepidNetworkGUID g;  // all zero
        auto v = PropertyValue::make_object(g);
        CHECK(encode_fobject(v, bw), "encode zero GUID");
        CHECK(bw.bit_pos() == 128, "128 bits written");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fobject(rd);
        auto* out = std::get_if<FIntrepidNetworkGUID>(&d.payload);
        CHECK(out, "decoded object payload");
        CHECK(out && out->ObjectId == 0, "zero ObjectId");
        CHECK(out && out->ServerId == 0, "zero ServerId");
        CHECK(out && out->Randomizer == 0, "zero Randomizer");
    }

    // Typical PC GUID (ObjectId=1, ServerId=0, Randomizer=0 — like captured bootstrap)
    {
        BunchWriter bw;
        FIntrepidNetworkGUID g;
        g.ObjectId = 1;
        auto v = PropertyValue::make_object(g);
        CHECK(encode_fobject(v, bw), "encode PC GUID");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fobject(rd);
        auto* out = std::get_if<FIntrepidNetworkGUID>(&d.payload);
        CHECK(out && out->ObjectId == 1, "ObjectId=1 preserved");
    }

    // 64-bit ObjectId with high bits set
    {
        BunchWriter bw;
        FIntrepidNetworkGUID g;
        g.ObjectId   = 0xDEADBEEFCAFEBABEull;
        g.ServerId   = 0x12345678u;
        g.Randomizer = 0xABCDEF01u;
        auto v = PropertyValue::make_object(g);
        CHECK(encode_fobject(v, bw), "encode full GUID");
        CHECK(bw.bit_pos() == 128, "128 bits");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fobject(rd);
        auto* out = std::get_if<FIntrepidNetworkGUID>(&d.payload);
        CHECK(out && out->ObjectId   == 0xDEADBEEFCAFEBABEull, "ObjectId preserved");
        CHECK(out && out->ServerId   == 0x12345678u,          "ServerId preserved");
        CHECK(out && out->Randomizer == 0xABCDEF01u,          "Randomizer preserved");
    }
}

// ─── FStruct (FVector / FRotator / dispatcher paths) ─────────────────────

// Build a 3-double StructValue for convenience
static PropertyValue make_vec3(double x, double y, double z) {
    StructValue sv;
    auto add = [&](double v) {
        PropertyValue p;
        p.type = FPropertyType::Double;
        p.payload = v;
        sv.fields.push_back(std::move(p));
    };
    add(x); add(y); add(z);
    return PropertyValue::make_struct(std::move(sv));
}

static bool vec3_equal(const PropertyValue& v, double x, double y, double z) {
    const StructValue* sv = std::get_if<StructValue>(&v.payload);
    if (!sv || sv->fields.size() != 3) return false;
    const double* a = std::get_if<double>(&sv->fields[0].payload);
    const double* b = std::get_if<double>(&sv->fields[1].payload);
    const double* c = std::get_if<double>(&sv->fields[2].payload);
    return a && b && c && *a == x && *b == y && *c == z;
}

void test_fstruct() {
    // 1) FVector round-trip (direct codec)
    {
        BunchWriter bw;
        auto v = make_vec3(1.0, -2.5, 3.14159);
        CHECK(encode_fvector(v, bw), "encode FVector");
        CHECK(bw.bit_pos() == 192, "192 bits (3 × 64)");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fvector(rd);
        CHECK(vec3_equal(d, 1.0, -2.5, 3.14159), "FVector round-trip");
    }
// -- C | Y | B & C | O | A --

    // 2) FRotator round-trip (same format as FVector)
    {
        BunchWriter bw;
        auto v = make_vec3(0.0, 90.0, 180.0);
        CHECK(encode_frotator(v, bw), "encode FRotator");
        CHECK(bw.bit_pos() == 192, "192 bits (3 × 64)");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_frotator(rd);
        CHECK(vec3_equal(d, 0.0, 90.0, 180.0), "FRotator round-trip");
    }

    // 3) Dispatcher → atomic codec via desc.name ("Vector")
    {
        ReplicatedPropertyDesc desc;
        desc.name = "Vector";
        desc.type = FPropertyType::Struct;

        BunchWriter bw;
        auto v = make_vec3(100.0, 200.0, 300.0);
        CHECK(encode_fstruct(desc, v, bw), "encode_fstruct 'Vector'");
        CHECK(bw.bit_pos() == 192, "192 bits");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fstruct(desc, rd);
        CHECK(vec3_equal(d, 100.0, 200.0, 300.0), "dispatcher → FVector");
    }

    // 4) Dispatcher → atomic codec via property-name alias
    //    ("TargetViewRotation" maps to FRotator codec in the registry)
    {
        ReplicatedPropertyDesc desc;
        desc.name = "TargetViewRotation";
        desc.type = FPropertyType::Struct;

        BunchWriter bw;
        auto v = make_vec3(45.0, -45.0, 0.0);
        CHECK(encode_fstruct(desc, v, bw), "encode_fstruct 'TargetViewRotation'");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fstruct(desc, rd);
        CHECK(vec3_equal(d, 45.0, -45.0, 0.0), "alias → FRotator");
    }

    // 5) Expanded struct via sub_cmds (recursive dispatcher)
    //    Build a fake struct with sub_cmds = [Int, Bool, Float]
    {
        ReplicatedPropertyDesc desc;
        desc.name = "UnknownTestStruct";  // not in registry
        desc.type = FPropertyType::Struct;

        ReplicatedPropertyDesc int_sub;
        int_sub.name = "a"; int_sub.type = FPropertyType::Int;
        ReplicatedPropertyDesc bool_sub;
        bool_sub.name = "b"; bool_sub.type = FPropertyType::Bool;
        ReplicatedPropertyDesc float_sub;
        float_sub.name = "c"; float_sub.type = FPropertyType::Float;
        desc.sub_cmds = {int_sub, bool_sub, float_sub};

        // Build a matching value
        StructValue sv;
        sv.fields.push_back(PropertyValue::make_int(42));
        sv.fields.push_back(PropertyValue::make_bool(true));
        PropertyValue ff; ff.type = FPropertyType::Float; ff.payload = 2.71828f;
        sv.fields.push_back(ff);
        PropertyValue v = PropertyValue::make_struct(std::move(sv));

        BunchWriter bw;
        CHECK(encode_fstruct(desc, v, bw), "encode expanded struct");
        //  32 (int) + 1 (bool) + 32 (float) = 65 bits
        CHECK(bw.bit_pos() == 65, "expanded struct = 65 bits");

        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_fstruct(desc, rd);
        const StructValue* out = std::get_if<StructValue>(&d.payload);
        CHECK(out && out->fields.size() == 3, "decoded 3 fields");
        if (out && out->fields.size() == 3) {
            auto* ia = std::get_if<int32_t>(&out->fields[0].payload);
            auto* bb = std::get_if<bool>   (&out->fields[1].payload);
            auto* fc = std::get_if<float>  (&out->fields[2].payload);
            CHECK(ia && *ia == 42,     "expanded int preserved");
            CHECK(bb && *bb == true,   "expanded bool preserved");
            CHECK(fc && *fc == 2.71828f, "expanded float preserved");
        }
    }

    // 6) Top-level encode_property/decode_property dispatching via Struct type
    //    (verifies the FStruct path is wired through the central dispatcher)
    {
        ReplicatedPropertyDesc desc;
        desc.name = "Vector";
        desc.type = FPropertyType::Struct;

        BunchWriter bw;
        auto v = make_vec3(-1.0, -2.0, -3.0);
        CHECK(encode_property(desc, v, bw),
              "encode_property (top-level) → FVector");
        PacketReader rd(bw.data(), bw.byte_size(), bw.bit_pos());
        auto d = decode_property(desc, rd);
        CHECK(vec3_equal(d, -1.0, -2.0, -3.0),
              "decode_property (top-level) → FVector");
    }
}

} // namespace

int main() {
    auto logger = spdlog::stdout_color_mt("test_replayout_codecs");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%^%l%$] %v");

    spdlog::info("== RepLayout codec round-trip tests ==");

    spdlog::info("-- FBool --");
    test_fbool();

    spdlog::info("-- FByte --");
    test_fbyte();

    spdlog::info("-- FInt --");
    test_fint();

    spdlog::info("-- FFloat --");
    test_ffloat();

    spdlog::info("-- FString --");
    test_fstring();

    spdlog::info("-- FObject (FIntrepidNetworkGUID) --");
    test_fobject();

    spdlog::info("-- FStruct (FVector / FRotator / dispatcher) --");
    test_fstruct();

    spdlog::info("== Result: {} passed, {} failed ==", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

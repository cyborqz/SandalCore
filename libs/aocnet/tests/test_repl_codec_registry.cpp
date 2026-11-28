// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_codec_registry.cpp
//
//  Tests for CustomStruct in Value (value.h), StructCodecRegistry
//  (struct_codec_registry.{h,cpp}), fallback dispatch in codecs.cpp, and
//  end-to-end integration with RepState, RepBlock, RepShadow, dynamic array
//  zero-reset, and Rpc parameter serialization.
// ============================================================================
#include "protocol/repl/codecs.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/rep_state.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/struct_codec_registry.h"
#include "protocol/repl/value.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using wire::PacketReader;
namespace schema = aoc::protocol::schema;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

namespace {

struct TestPoint {
    int32_t     x = 0;
    int32_t     y = 0;
    std::string tag;
    bool operator==(const TestPoint&) const = default;
};

struct TestOther {
    int32_t     x = 0;
    int32_t     y = 0;
    std::string tag;
    bool operator==(const TestOther&) const = default;
};

struct DefaultFloat {
    float f = 0.0f;
    bool operator==(const DefaultFloat&) const = default;
};

struct BitFloat {
    float f = 0.0f;
    bool operator==(const BitFloat& o) const { return same_bits(f, o.f); }
};

struct TestNoZero {
    uint32_t token = 0;
    bool operator==(const TestNoZero&) const = default;
};

struct MoveOnlyCustom {
    int32_t value = 0;
    MoveOnlyCustom() = default;
    explicit MoveOnlyCustom(int32_t v) : value(v) {}
    MoveOnlyCustom(const MoveOnlyCustom&) = delete;
    MoveOnlyCustom& operator=(const MoveOnlyCustom&) = delete;
    MoveOnlyCustom(MoveOnlyCustom&&) noexcept = default;
    MoveOnlyCustom& operator=(MoveOnlyCustom&&) noexcept = default;
    bool operator==(const MoveOnlyCustom& o) const { return value == o.value; }
};

struct NonEq {
    int32_t x = 0;
};

struct NonConstEq {
    int32_t x = 0;
    bool operator==(NonConstEq&) { return true; }
};

enum UnscopedEnum { kUnscopedZero = 0 };
enum class ScopedEnum : uint8_t { kScopedZero = 0 };

// Compile-time guards on Value construction and size.
static_assert(!std::is_constructible_v<Value, bool>);
static_assert(!std::is_constructible_v<Value, int>);
static_assert(!std::is_constructible_v<Value, int64_t>);
static_assert(!std::is_constructible_v<Value, uint64_t>);
static_assert(!std::is_constructible_v<Value, float>);
static_assert(!std::is_constructible_v<Value, double>);
static_assert(!std::is_constructible_v<Value, UnscopedEnum>);
static_assert(!std::is_constructible_v<Value, ScopedEnum>);
static_assert(!std::is_constructible_v<Value, int*>);
static_assert(!std::is_constructible_v<Value, const char*>);
static_assert(!std::is_constructible_v<Value, std::nullptr_t>);
static_assert(!std::is_constructible_v<Value, std::string_view>);
static_assert(!std::is_constructible_v<Value, NonEq>);
static_assert(!std::is_constructible_v<Value, NonConstEq>);
static_assert(!std::is_constructible_v<Value, CustomStruct>);
static_assert(!std::is_constructible_v<Value, MoveOnlyCustom&>);
static_assert(std::is_constructible_v<Value, MoveOnlyCustom&&>);
static_assert(std::is_constructible_v<Value, TestPoint>);
static_assert(std::is_constructible_v<Value, const TestPoint&>);
static_assert(std::is_constructible_v<Value, Vec3d>);
static_assert(std::is_constructible_v<Value, ItemStorageEvent>);
static_assert(std::is_constructible_v<Value, MailPersistenceData>);

// CustomStruct must not grow Value beyond its existing largest alternative.
static_assert(sizeof(CustomStruct) <= sizeof(ItemInstance));
static_assert(sizeof(Value) == sizeof(Value::Storage));

// Non-const Value::get_if<Custom>() resolves to the const overload (returns const Custom*),
// while built-in Storage types preserve mutable T* access.
static_assert(std::is_same_v<decltype(std::declval<Value&>().get_if<TestPoint>()), const TestPoint*>);
static_assert(std::is_same_v<decltype(std::declval<const Value&>().get_if<TestPoint>()), const TestPoint*>);
static_assert(std::is_same_v<decltype(std::declval<Value&>().get_if<Vec3d>()), Vec3d*>);
static_assert(std::is_same_v<decltype(std::declval<Value&>().get_if<ItemStorageEvent>()), ItemStorageEvent*>);
static_assert(std::is_same_v<decltype(std::declval<Value&>().get_if<MailPersistenceData>()), MailPersistenceData*>);

// ---- Test codecs ------------------------------------------------------------

bool accepts_test_point(const Value& v) {
    const auto* p = v.get_if<TestPoint>();
    return p != nullptr && p->x >= -1000 && p->x <= 1000 && fstring_fits(p->tag);
}

std::optional<Value> zero_test_point() {
    return Value(TestPoint{0, 0, ""});
}

// Uses opts.rep_movement_short_rotation to verify CodecOptions propagation:
// when true, writes/reads an extra 1-bit marker + 16-bit checksum tag.
std::optional<Value> read_test_point(PacketReader& r, const CodecOptions& opts) {
    TestPoint p;
    p.x = static_cast<int32_t>(r.read_uint32());
    p.y = static_cast<int32_t>(r.read_uint32());
    auto tag = read_fstring(r);
    if (!tag) return std::nullopt;
    p.tag = std::move(*tag);
    if (opts.rep_movement_short_rotation) {
        if (r.read_uint16() != 0xA55Au) return std::nullopt;
    }
    if (overran(r) || p.x < -1000 || p.x > 1000) return std::nullopt;
    return Value(std::move(p));
}

bool write_test_point(const Value& v, BunchWriter& out, const CodecOptions& opts) {
    const auto* p = v.get_if<TestPoint>();
    if (!p) return false;
    out.write_int32(p->x);
    out.write_int32(p->y);
    if (!write_fstring(out, p->tag)) return false;
    if (opts.rep_movement_short_rotation) {
        out.write_uint16(0xA55Au);
    }
    return true;
}

bool accepts_no_zero(const Value& v) {
    return v.get_if<TestNoZero>() != nullptr;
}

std::optional<Value> read_no_zero(PacketReader& r, const CodecOptions&) {
    TestNoZero z;
    z.token = r.read_uint32();
    if (overran(r)) return std::nullopt;
    return Value(z);
}

bool write_no_zero(const Value& v, BunchWriter& out, const CodecOptions&) {
    const auto* z = v.get_if<TestNoZero>();
    if (!z) return false;
    out.write_uint32(z->token);
    return true;
}

// Intentionally omits overran(r) check so we prove codecs.cpp read_struct fails closed.
std::optional<Value> read_sloppy_overrun(PacketReader& r, const CodecOptions&) {
    TestNoZero z;
    z.token = r.read_uint32();
    return Value(z);
}

StructCodecOps point_ops() {
    return StructCodecOps{
        .accepts = &accepts_test_point,
        .zero    = &zero_test_point,
        .read    = &read_test_point,
        .write   = &write_test_point,
    };
}

StructCodecOps no_zero_ops() {
    return StructCodecOps{
        .accepts = &accepts_no_zero,
        .zero    = nullptr,
        .read    = &read_no_zero,
        .write   = &write_no_zero,
    };
}

template <class Ex, class Fn>
bool throws_exception(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const Ex&) {
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

static void test_value_custom_struct() {
    std::printf("Value: CustomStruct lifecycle, equality, same_bits, and built-in parity\n");

    const TestPoint src{10, -20, "alpha"};
    Value v1(src);
    CHECK(v1.kind() == ValueKind::Struct, "custom struct Value::kind() is ValueKind::Struct");
    const TestPoint* p1 = v1.get_if<TestPoint>();
    CHECK(p1 != nullptr && *p1 == src, "get_if<TestPoint>() returns stored value");

    // Copy shares immutable payload and compares equal.
    Value v2 = v1;
    CHECK(v2.get_if<TestPoint>() == p1, "copy shares immutable underlying payload");
    CHECK(v1 == v2, "copied custom Value compares equal");

    // Distinct allocation with equal fields compares equal; differing fields do not.
    Value v3(TestPoint{10, -20, "alpha"});
    Value v4(TestPoint{10, -21, "alpha"});
    CHECK(v3.get_if<TestPoint>() != p1 && v1 == v3, "distinct allocations with equal fields compare equal");
    CHECK(!(v1 == v4), "differing fields compare not equal");

    // Cross-type isolation even with identical memory layout.
    Value other(TestOther{10, -20, "alpha"});
    CHECK(!(v1 == other), "distinct custom struct types never compare equal");
    CHECK(v1.get_if<TestOther>() == nullptr, "get_if<WrongCustom>() returns nullptr");
    CHECK(v1.get_if<Vec3d>() == nullptr, "get_if<BuiltIn>() on custom Value returns nullptr");
    CHECK(Value(Vec3d{1, 2, 3}).get_if<TestPoint>() == nullptr, "get_if<Custom>() on built-in Value returns nullptr");

    // Move-only custom struct moves cleanly into shared immutable Value, then copies freely.
    Value mv(MoveOnlyCustom{77});
    Value mv_copy = mv;
    CHECK(mv_copy.get_if<MoveOnlyCustom>() && mv_copy.get_if<MoveOnlyCustom>()->value == 77 && mv == mv_copy,
          "move-only custom struct wraps cleanly and copied Values stay equal");

    // Moved-from CustomStruct Value stays safe on get_if and equality.
    Value moved_src(TestPoint{3, 4, "move"});
    Value moved_dst = std::move(moved_src);
    CHECK(moved_dst.get_if<TestPoint>() && moved_dst.get_if<TestPoint>()->x == 3, "move destination retains custom value");
    CHECK(moved_src.get_if<TestPoint>() == nullptr, "moved-from custom Value returns nullptr from get_if");
    CHECK(!(moved_src == moved_dst) && moved_src == moved_src, "moved-from custom Value equality is safe and consistent");

    // No raw pointer short-circuit: T::operator== is always authoritative.
    const float qnan = std::bit_cast<float>(0x7fc00000u);
    Value nan_def_a(DefaultFloat{qnan});
    Value nan_def_b = nan_def_a;  // same shared_ptr!
    CHECK(nan_def_a.get_if<DefaultFloat>() == nan_def_b.get_if<DefaultFloat>() && !(nan_def_a == nan_def_b),
          "default float NaN does not falsely compare equal via pointer short-circuit");
    CHECK(Value(DefaultFloat{+0.0f}) == Value(DefaultFloat{-0.0f}),
          "default float +0.0 == -0.0 honored by CustomStruct");

    Value nan_bit_a(BitFloat{qnan});
    Value nan_bit_b = nan_bit_a;
    Value nan_bit_c(BitFloat{qnan});
    CHECK(nan_bit_a == nan_bit_b && nan_bit_a == nan_bit_c,
          "same_bits float NaN compares equal for same bit pattern");
    CHECK(!(Value(BitFloat{+0.0f}) == Value(BitFloat{-0.0f})),
          "same_bits distinguishes +0.0f from -0.0f for shadow comparisons");

    // Built-in types remain unaffected (kind, boxed types, mutable get_if).
    Value v_none;
    Value v_bool = Value::boolean(true);
    Value v_arr(ArrayValue{});
    Value v_inl(InlineObject{});
    Value v_vec(Vec3d{1.0, 2.0, 3.0});
    ItemStorageEvent ev;
    ev.type = 2;
    Value v_ev(ev);
    MailPersistenceData mail;
    mail.batch_num = 9;
    Value v_mail(mail);
    CHECK(v_none.kind() == ValueKind::None && v_bool.kind() == ValueKind::Bool &&
              v_arr.kind() == ValueKind::Array && v_inl.kind() == ValueKind::Object &&
              v_vec.kind() == ValueKind::Struct && v_ev.kind() == ValueKind::Struct &&
              v_mail.kind() == ValueKind::Struct,
          "built-in ValueKind mapping is unchanged");
    v_vec.get_if<Vec3d>()->x = 42.0;
    v_ev.get_if<ItemStorageEvent>()->type = 4;
    v_mail.get_if<MailPersistenceData>()->batch_num = 11;
    CHECK(v_vec.get_if<Vec3d>()->x == 42.0 && v_ev.get_if<ItemStorageEvent>()->type == 4 &&
              v_mail.get_if<MailPersistenceData>()->batch_num == 11,
          "mutable get_if still works for built-in and boxed types");
}

static void test_registry_errors_and_safety() {
    std::printf("StructCodecRegistry: validation, duplicate, built-in collision, and unknown lookup\n");

    const StructCodecOps valid = point_ops();

    CHECK(throws_exception<std::invalid_argument>([&]() { register_struct_codec("", valid); }),
          "empty struct name throws std::invalid_argument");

    StructCodecOps bad_accepts = valid;
    bad_accepts.accepts = nullptr;
    CHECK(throws_exception<std::invalid_argument>([&]() { register_struct_codec("FTestBadAccepts", bad_accepts); }),
          "null accepts callback throws std::invalid_argument");

    StructCodecOps bad_read = valid;
    bad_read.read = nullptr;
    CHECK(throws_exception<std::invalid_argument>([&]() { register_struct_codec("FTestBadRead", bad_read); }),
          "null read callback throws std::invalid_argument");

    StructCodecOps bad_write = valid;
    bad_write.write = nullptr;
    CHECK(throws_exception<std::invalid_argument>([&]() { register_struct_codec("FTestBadWrite", bad_write); }),
          "null write callback throws std::invalid_argument");

    CHECK(throws_exception<std::logic_error>([&]() { register_struct_codec("FVector", valid); }),
          "shadowing built-in FVector throws std::logic_error");
    CHECK(throws_exception<std::logic_error>([&]() { register_struct_codec("FItemInstance", valid); }),
          "shadowing built-in FItemInstance throws std::logic_error");
    CHECK(throws_exception<std::logic_error>([&]() { register_struct_codec("FCurrencyContainer", valid); }),
          "shadowing built-in FCurrencyContainer throws std::logic_error");

    // Duplicate registration of a custom struct name throws std::logic_error.
    register_struct_codec("FTestDup", valid);
    CHECK(throws_exception<std::logic_error>([&]() { register_struct_codec("FTestDup", valid); }),
          "duplicate registration of custom struct throws std::logic_error");

    // Unknown struct queries fail closed cleanly.
    CHECK(!is_known_struct("FTestNeverRegistered"), "unknown struct is_known_struct == false");
    CHECK(find_registered_struct_codec("FTestNeverRegistered") == nullptr, "unknown struct lookup == nullptr");
    CHECK(!struct_accepts("FTestNeverRegistered", Value(TestPoint{})), "unknown struct struct_accepts == false");
    CHECK(!zero_struct("FTestNeverRegistered").has_value(), "unknown struct zero_struct == nullopt");
    {
        uint8_t dummy[8] = {};
        PacketReader r(dummy, sizeof(dummy), 64);
        CHECK(!read_struct("FTestNeverRegistered", r).has_value(), "unknown struct read_struct == nullopt");
    }
    {
        BunchWriter out;
        CHECK(!write_struct("FTestNeverRegistered", Value(TestPoint{}), out) && out.bit_pos() == 0,
              "unknown struct write_struct == false and writes 0 bits");
    }

    // Built-in structs without a zero form still return nullopt (never overridden by registry).
    CHECK(!zero_struct("FRepMovement").has_value() && !zero_struct("FItemInstance").has_value(),
          "built-in structs without zero form still return nullopt");
}

static void test_codecs_dispatch() {
    std::printf("codecs.h dispatch: is_known_struct, struct_accepts, zero_struct, read_struct, write_struct\n");

    register_struct_codec("FTestPoint", point_ops());
    register_struct_codec("FTestNoZero", no_zero_ops());
    register_struct_codec("FTestSloppyOverrun", StructCodecOps{
        .accepts = &accepts_no_zero,
        .zero    = nullptr,
        .read    = &read_sloppy_overrun,
        .write   = &write_no_zero,
    });

    CHECK(is_known_struct("FTestPoint") && is_known_struct("FTestNoZero"),
          "registered custom structs report is_known_struct == true");

    // struct_accepts: valid, wrong custom type, wrong built-in type, out-of-bounds value.
    CHECK(struct_accepts("FTestPoint", Value(TestPoint{123, -456, "valid"})), "struct_accepts accepts valid custom value");
    CHECK(!struct_accepts("FTestPoint", Value(TestOther{123, -456, "valid"})), "struct_accepts rejects wrong custom type");
    CHECK(!struct_accepts("FTestPoint", Value(Vec3d{1, 2, 3})), "struct_accepts rejects wrong built-in type");
    CHECK(!struct_accepts("FTestPoint", Value(TestPoint{5000, 0, "oob"})), "struct_accepts rejects out-of-bounds value");

    // zero_struct: with callback and with nullptr callback.
    const auto zp = zero_struct("FTestPoint");
    CHECK(zp.has_value() && *zp == Value(TestPoint{0, 0, ""}), "zero_struct invokes zero callback when provided");
    CHECK(!zero_struct("FTestNoZero").has_value(), "zero_struct returns nullopt when zero callback is nullptr");

    // Round-trip with default CodecOptions.
    {
        const Value orig(TestPoint{-42, 99, "hello"});
        BunchWriter w;
        CHECK(write_struct("FTestPoint", orig, w) && w.bit_pos() > 0, "write_struct encodes custom struct");
        PacketReader r(w.data(), w.byte_size(), w.bit_pos());
        const auto decoded = read_struct("FTestPoint", r);
        CHECK(decoded.has_value() && r.pos() == w.bit_pos() && *decoded == orig,
              "read_struct round-trips custom struct to the last bit");
    }

    // CodecOptions propagation into both write and read.
    {
        CodecOptions opts;
        opts.rep_movement_short_rotation = true;
        const Value orig(TestPoint{7, 8, "opts"});
        BunchWriter w;
        CHECK(write_struct("FTestPoint", orig, w, opts), "write_struct propagates CodecOptions");
        PacketReader r_ok(w.data(), w.byte_size(), w.bit_pos());
        const auto decoded = read_struct("FTestPoint", r_ok, opts);
        CHECK(decoded.has_value() && r_ok.pos() == w.bit_pos() && *decoded == orig,
              "read_struct propagates CodecOptions and decodes exact bits");

        // Reading opts-encoded bits without opts stops 16 bits early (marker unused).
        PacketReader r_default(w.data(), w.byte_size(), w.bit_pos());
        const auto partial = read_struct("FTestPoint", r_default);
        CHECK(partial.has_value() && r_default.pos() + 16 == w.bit_pos(),
              "CodecOptions changed actual wire format by 16 bits");
    }

    // write_struct rejects bad value without modifying BunchWriter.
    {
        BunchWriter w;
        w.write_bits(0x5u, 3);
        const size_t before = w.bit_pos();
        CHECK(!write_struct("FTestPoint", Value(TestPoint{2000, 0, "bad"}), w) && w.bit_pos() == before,
              "write_struct rejects invalid custom value without writing bits");
        CHECK(!write_struct("FTestPoint", Value(TestOther{1, 2, "wrong"}), w) && w.bit_pos() == before,
              "write_struct rejects wrong custom type without writing bits");
    }

    // Truncated bitstream overrun fails closed — even when custom reader forgets overran(r).
    {
        uint8_t short_buf[2] = {0x12, 0x34};
        PacketReader r1(short_buf, sizeof(short_buf), 16);
        CHECK(!read_struct("FTestPoint", r1).has_value(), "truncated read_struct returns nullopt");

        PacketReader r2(short_buf, sizeof(short_buf), 16);
        CHECK(!read_struct("FTestSloppyOverrun", r2).has_value(),
              "codecs.cpp read_struct fails closed on overrun even if custom reader omits overran(r)");
    }
}

static void test_rep_block_and_rpc_integration() {
    std::printf("RepState + RepBlock + RepShadow + ArrayValue + Rpc with custom registered structs\n");

    using schema::ClassSchema;
    using schema::PropType;
    using schema::RepCommand;
    using schema::RpcParam;
    using schema::RpcSchema;

    static constexpr RepCommand kElemCmds[] = {
        {1, 0, "ElemPoint",  "ATestCustom", PropType::NetSerializeStruct, schema::kRepNone, 0, "FTestPoint",  nullptr, 0, 0},
        {1, 0, "ElemNoZero", "ATestCustom", PropType::NetSerializeStruct, schema::kRepNone, 0, "FTestNoZero", nullptr, 0, 0},
    };
    static constexpr RepCommand kClassCmds[] = {
        {1, 0, "CustomProp",       "ATestCustom", PropType::NetSerializeStruct, schema::kRepNone, 0, "FTestPoint", nullptr, 0, 0},
        {2, 1, "ZeroableArray",    "ATestCustom", PropType::DynamicArray,       schema::kRepNone, 0, nullptr,      nullptr, 0, 1},
        {3, 2, "NonZeroableArray", "ATestCustom", PropType::DynamicArray,       schema::kRepNone, 0, nullptr,      nullptr, 1, 2},
    };
    // Sorted alphabetically by path: "CustomProp" (1), "NonZeroableArray" (3), "ZeroableArray" (2).
    static constexpr uint16_t kCmdsByPath[] = {1, 3, 2};

    const ClassSchema cls{
        .name             = "ATestCustom",
        .super_name       = "AActor",
        .is_actor         = true,
        .first_own_handle = 1,
        .first_own_field  = 0,
        .commands         = kClassCmds,
        .element_commands = kElemCmds,
        .fields           = {},
        .rpcs             = {},
        .commands_by_path = kCmdsByPath,
        .fields_by_name   = {},
    };

    RepState state;
    CHECK(state.set(cls, "CustomProp", Value(TestPoint{11, 22, "first"})),
          "RepState::set accepts valid registered custom struct");
    CHECK(!state.set(cls, "CustomProp", Value(TestPoint{9999, 0, "oob"})),
          "RepState::set rejects out-of-bounds custom struct");
    CHECK(!state.set(cls, "CustomProp", Value(TestOther{11, 22, "wrong"})),
          "RepState::set rejects wrong custom struct type");
    CHECK(!state.set(cls, "CustomProp", Value(Vec3d{1, 2, 3})),
          "RepState::set rejects wrong built-in struct type");
    CHECK(*state.get(cls, "CustomProp") == Value(TestPoint{11, 22, "first"}),
          "failed RepState::set calls leave previous custom value intact");

    // First write_rep_block writes bits and decodes bit-exact.
    RepShadow shadow;
    BunchWriter w1;
    bool failed = false;
    const size_t bits1 = write_rep_block(cls, state, shadow, w1, {}, &failed);
    CHECK(!failed && bits1 > 0 && bits1 == w1.bit_pos(), "first write_rep_block writes custom struct bits");
    {
        size_t pos = 0;
        const auto decoded = read_rep_block(cls, w1.data(), w1.bit_pos(), pos);
        CHECK(decoded.has_value() && pos == w1.bit_pos() && *decoded == state,
              "read_rep_block decodes custom struct back to equal RepState");
    }

    // Second write_rep_block with unchanged shadow writes 0 bits (also with fresh equal allocation).
    BunchWriter w2;
    CHECK(write_rep_block(cls, state, shadow, w2, {}, &failed) == 0 && !failed && w2.bit_pos() == 0,
          "unchanged shadow writes 0 bits for custom struct");
    state.set(cls, "CustomProp", Value(TestPoint{11, 22, "first"}));
    CHECK(write_rep_block(cls, state, shadow, w2, {}, &failed) == 0 && !failed && w2.bit_pos() == 0,
          "re-setting equal custom struct (new allocation) still writes 0 bits");

    // Mutated custom struct writes delta bits and decodes to new value.
    state.set(cls, "CustomProp", Value(TestPoint{33, 44, "second"}));
    BunchWriter w3;
    const size_t bits3 = write_rep_block(cls, state, shadow, w3, {}, &failed);
    CHECK(!failed && bits3 > 0, "mutated custom struct writes delta bits");
    {
        size_t pos = 0;
        const auto decoded = read_rep_block(cls, w3.data(), w3.bit_pos(), pos);
        CHECK(decoded.has_value() && pos == w3.bit_pos() &&
                  decoded->get(cls, "CustomProp") &&
                  *decoded->get(cls, "CustomProp") == Value(TestPoint{33, 44, "second"}),
              "read_rep_block decodes mutated custom struct accurately");
    }

    // Dynamic array with zeroable custom struct: element removal re-sends zero_struct.
    {
        ArrayValue arr;
        ElementState elem;
        elem.values.emplace(1, Value(TestPoint{5, 6, "elem"}));
        arr.elements.push_back(std::move(elem));
        CHECK(state.set(cls, "ZeroableArray", Value(std::move(arr))),
              "RepState::set accepts dynamic array carrying registered custom struct");

        BunchWriter wa1;
        const size_t abits1 = write_rep_block(cls, state, shadow, wa1, {}, &failed);
        CHECK(!failed && abits1 > 0, "write_rep_block writes custom struct array element");

        // Remove local 1 from element 0 while keeping array length 1 -> triggers zero_struct("FTestPoint").
        ArrayValue cleared;
        cleared.elements.emplace_back();
        CHECK(state.set(cls, "ZeroableArray", Value(std::move(cleared))),
              "RepState::set accepts cleared element in zeroable custom struct array");

        BunchWriter wa2;
        const size_t abits2 = write_rep_block(cls, state, shadow, wa2, {}, &failed);
        CHECK(!failed && abits2 > 0, "removing zeroable custom struct array element sends zero_struct delta");
        size_t pos = 0;
        const auto decoded = read_rep_block(cls, wa2.data(), wa2.bit_pos(), pos);
        const ArrayValue* darr = decoded && decoded->get(cls, "ZeroableArray")
            ? decoded->get(cls, "ZeroableArray")->get_if<ArrayValue>()
            : nullptr;
        CHECK(darr && darr->elements.size() == 1 && darr->elements[0].values.count(1) == 1 &&
                  darr->elements[0].values.at(1) == Value(TestPoint{0, 0, ""}),
              "removed zeroable custom struct array element decodes as zero_struct");
    }

    // Dynamic array with non-zeroable custom struct (zero == nullptr): element removal fails loudly.
    {
        ArrayValue arr;
        ElementState elem;
        elem.values.emplace(1, Value(TestNoZero{0x12345678u}));
        arr.elements.push_back(std::move(elem));
        CHECK(state.set(cls, "NonZeroableArray", Value(std::move(arr))),
              "RepState::set accepts dynamic array carrying non-zeroable custom struct");

        BunchWriter wnz1;
        CHECK(write_rep_block(cls, state, shadow, wnz1, {}, &failed) > 0 && !failed,
              "write_rep_block writes non-zeroable custom struct array element");

        ArrayValue cleared;
        cleared.elements.emplace_back();
        state.set(cls, "NonZeroableArray", Value(std::move(cleared)));

        BunchWriter wnz2;
        const RepShadow shadow_before = shadow;
        const size_t bits_fail = write_rep_block(cls, state, shadow, wnz2, {}, &failed);
        CHECK(failed && bits_fail == 0 && wnz2.bit_pos() == 0 && shadow.sent == shadow_before.sent,
              "removing non-zeroable custom struct element fails loudly with out and shadow unchanged");
    }

    // RPC parameter round-trip with a registered custom NetSerializeStruct.
    {
        static constexpr RpcParam kRpcParams[] = {
            {"Payload", PropType::NetSerializeStruct, 0, "FTestPoint", true,  0, 1},
            {"Flag",    PropType::Bool,               1, nullptr,      false, 1, 2},
        };
        static constexpr RepCommand kRpcCmds[] = {
            {1, 0, "Payload", "ATestCustom", PropType::NetSerializeStruct, schema::kRepNone, 0, "FTestPoint", nullptr, 0, 0},
            {2, 1, "Flag",    "ATestCustom", PropType::Bool,               schema::kRepNone, 1, nullptr,      nullptr, 0, 0},
        };
        const RpcSchema rpc{
            .name             = "ServerSendCustom",
            .owner            = "ATestCustom",
            .function_flags   = schema::kFuncNet | schema::kFuncNetServer | schema::kFuncNetReliable,
            .field_index      = 0,
            .params           = kRpcParams,
            .commands         = kRpcCmds,
            .element_commands = {},
        };

        RpcParams params;
        CHECK(!set_rpc_param(rpc, params, "Payload", Value(TestOther{1, 2, "bad"})),
              "set_rpc_param rejects wrong custom struct type");
        CHECK(!set_rpc_param(rpc, params, "Payload", Value(TestPoint{9999, 0, "oob"})),
              "set_rpc_param rejects out-of-bounds custom struct");
        CHECK(set_rpc_param(rpc, params, "Payload", Value(TestPoint{-77, 88, "rpc_ok"})),
              "set_rpc_param accepts valid registered custom struct");
        CHECK(set_rpc_param(rpc, params, "Flag", Value::boolean(true)),
              "set_rpc_param accepts companion bool param");

        BunchWriter wrpc;
        const size_t rpc_bits = write_rpc_params(rpc, params, wrpc, {}, &failed);
        CHECK(!failed && rpc_bits > 0 && rpc_bits == wrpc.bit_pos(), "write_rpc_params encodes custom struct param");

        size_t pos = 0;
        const auto decoded = read_rpc_params(rpc, wrpc.data(), wrpc.bit_pos(), pos);
        CHECK(decoded.has_value() && pos == wrpc.bit_pos() && *decoded == params,
              "read_rpc_params round-trips custom struct param bit-exact");
    }
}

static void test_pointer_stability_and_concurrency() {
    std::printf("StructCodecRegistry: pointer stability and multi-threaded concurrent access\n");

    register_struct_codec("FTestStableA", point_ops());
    const StructCodecOps* stable_ptr = find_registered_struct_codec("FTestStableA");
    CHECK(stable_ptr != nullptr, "find_registered_struct_codec returns non-null pointer");

    for (int i = 0; i < 25; ++i) {
        register_struct_codec("FTestStableExtra" + std::to_string(i), no_zero_ops());
    }
    const StructCodecOps* after_ptr = find_registered_struct_codec("FTestStableA");
    CHECK(after_ptr == stable_ptr && stable_ptr->accepts(Value(TestPoint{1, 2, "stable"})),
          "pointer returned by find_registered_struct_codec stays stable after 25 additional registrations");

    // Multi-threaded concurrent lookup + read/write + concurrent distinct registrations.
    constexpr int kReaderThreads = 6;
    constexpr int kIterations = 400;
    std::atomic<bool> go{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kReaderThreads + 1);

    for (int t = 0; t < kReaderThreads; ++t) {
        threads.emplace_back([&, t]() {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kIterations; ++i) {
                if (!is_known_struct("FTestStableA") || is_known_struct("FTestConcurrentUnknown")) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (find_registered_struct_codec("FTestStableA") != stable_ptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                const Value v(TestPoint{t * 10 + (i % 7), -i, "mt"});
                BunchWriter w;
                if (!write_struct("FTestStableA", v, w)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                PacketReader r(w.data(), w.byte_size(), w.bit_pos());
                const auto back = read_struct("FTestStableA", r);
                if (!back || r.pos() != w.bit_pos() || !(*back == v)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }

    threads.emplace_back([&]() {
        while (!go.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 12; ++i) {
            try {
                register_struct_codec("FTestConcurrentReg" + std::to_string(i), no_zero_ops());
            } catch (...) {
                failures.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    });

    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    CHECK(failures.load() == 0 && is_known_struct("FTestConcurrentReg11"),
          "6 concurrent reader threads + concurrent registrar completed with zero errors");
}

int main() {
    test_value_custom_struct();
    test_registry_errors_and_safety();
    test_codecs_dispatch();
    test_rep_block_and_rpc_integration();
    test_pointer_stability_and_concurrency();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

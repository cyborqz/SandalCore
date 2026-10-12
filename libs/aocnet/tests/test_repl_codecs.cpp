// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_codecs.cpp
//
//  The NetSerialize codecs and value primitives of protocol/repl/codecs.h,
//  bit for bit against single values the original server sent (capture_blocks.h,
//  fixtures/replay_data.bin): FVector, FRepMovement, FVector_NetQuantize100,
//  FRotator, FUniqueNetIdRepl, FCharacterLoadTracker, FGameplayTag, FText,
//  soft object, FName and FString.  Each captured value is read to its last bit,
//  checked against what tools/ghidra/schema_check.py decodes, and written back
//  to the same bits.  Codecs the capture holds no example of (FVector2D,
//  FDateTime, FVector_NetQuantizeNormal, FGameplayTagContainer, the raw
//  quantized forms, UCS-2 strings) are checked by round trip only.  Also the limits
//  of a string, FRepMovement's quantization levels and the zero value of a struct.
// ============================================================================
#include "capture_blocks.h"
#include "protocol/repl/codecs.h"

#include <cmath>
#include <cstdio>
#include <functional>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using wire::PacketReader;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

using Reader = std::function<std::optional<Value>(PacketReader&)>;
using Writer = std::function<bool(const Value&, BunchWriter&)>;
// -- C-Y-B x CoA --

/// Reads `s` with `read`; true when it consumed exactly s.bits and writing the
/// value back gives the same bits.  *out gets the decoded value.
static bool captured_round_trip(const capture_blocks::Slice& s, const Reader& read, const Writer& write, Value* out) {
    const auto bytes = capture_blocks::bytes_of(s.hex);
    PacketReader r(bytes.data(), bytes.size(), s.bits);
    const auto v = read(r);
    if (!v || r.pos() != s.bits) return false;
    *out = *v;
    BunchWriter w;
    if (!write(*v, w) || w.bit_pos() != s.bits) return false;
    size_t diff = 0;
    if (!capture_blocks::same_bit_range(w.data(), bytes.data(), s.bits, &diff)) {
        std::printf("    first differing bit %zu\n", diff);
        return false;
    }
    return true;
}

static Reader struct_reader(const char* name, const CodecOptions& opts = {}) {
    return [name, opts](PacketReader& r) { return read_struct(name, r, opts); };
}
static Writer struct_writer(const char* name, const CodecOptions& opts = {}) {
    return [name, opts](const Value& v, BunchWriter& w) { return write_struct(name, v, w, opts); };
}

/// Writes `v` as `name` and reads it back.
static std::optional<Value> struct_trip(const char* name, const Value& v, const CodecOptions& opts = {}) {
    BunchWriter w;
    if (!write_struct(name, v, w, opts)) return std::nullopt;
    PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    auto back = read_struct(name, r, opts);
    if (!back || r.pos() != w.bit_pos()) return std::nullopt;
    return back;
}

static void test_fvector() {
    std::printf("FVector: PC SpawnLocation, 3 doubles\n");
    Value v;
    CHECK(captured_round_trip(capture_blocks::kVectorSpawnLocation, struct_reader("FVector"), struct_writer("FVector"), &v),
          "192 bits read to the last bit and written back identically");
    const Vec3d* d = v.get_if<Vec3d>();
    CHECK(d && d->x == -777664.5 && d->y == 616578.6875 && d->z == 15952.0498046875, "(-777664.5, 616578.6875, 15952.0498046875)");
}

static void test_rep_movement() {
    std::printf("FRepMovement: pawn ReplicatedMovement at open (byte rotation)\n");
    Value v;
    CHECK(captured_round_trip(capture_blocks::kRepMovementOpen, struct_reader("FRepMovement"), struct_writer("FRepMovement"), &v),
          "117 bits read to the last bit and written back identically");
    const RepMovement* m = v.get_if<RepMovement>();
    CHECK(m && !m->simulated_physic_sleep && !m->rep_physics && !m->server_frame && !m->server_physics_handle && !m->acceleration,
          "no flags: no sleep, physics, server frame, handle or acceleration");
    CHECK(m && m->location.mode == QuantVec::Mode::Scaled && m->location.x == -77766450 && m->location.y == 61657869 &&
              m->location.z == 1595205,
          "location (-77766450, 61657869, 1595205) at scale 100");
    CHECK(m && m->rotation.pitch == 0 && m->rotation.yaw == 65 && m->rotation.roll == 0, "rotation yaw 65 (byte components)");
    CHECK(m && m->linear_velocity.x == 0 && m->linear_velocity.y == 0 && m->linear_velocity.z == 0, "no velocity");
    if (m) {
        const Vec3d p = dequantize_vector(m->location, 100);
        CHECK(std::fabs(p.x - -777664.5) < 1e-6 && std::fabs(p.y - 616578.69) < 1e-6 && std::fabs(p.z - 15952.05) < 1e-6,
              "location / 100 is the PC's SpawnLocation (-777664.5, 616578.69, 15952.05)");
        CHECK(quantize_vector(100, p.x, p.y, p.z) == m->location, "quantize_vector at 100 gives the captured integers back");
    }
    // The same bits read as short rotation components cannot come out (the block
    // would end mid-value): the rotation width is a per-object setting.
    CodecOptions shorts;
    shorts.rep_movement_short_rotation = true;
    Value ignored;
    CHECK(!captured_round_trip(capture_blocks::kRepMovementOpen, struct_reader("FRepMovement", shorts),
                               struct_writer("FRepMovement", shorts), &ignored),
          "read with ShortComponents it is not the captured 117 bits");
}

static void test_rep_movement_forms() {
    std::printf("FRepMovement: physics, server frame and handle, acceleration, short rotation\n");
    RepMovement m;
    m.simulated_physic_sleep = true;
    m.rep_physics = true;
    m.location = quantize_vector(100, 1234.5, -20.25, 3.0);
    m.rotation = {100, 0, 300};
    m.linear_velocity = quantize_vector(1, 600, 0, -980);
    m.angular_velocity = quantize_vector(1, 0, 0, 90);
    m.server_frame = 123456;
    m.server_physics_handle = 7;
    m.acceleration = quantize_vector(1, 100, 0, 0);
    CodecOptions shorts;
    shorts.rep_movement_short_rotation = true;
    const auto back = struct_trip("FRepMovement", Value(m), shorts);
    const RepMovement* b = back ? back->get_if<RepMovement>() : nullptr;
    CHECK(b && *b == m, "every optional part round trips with short rotation components");
    BunchWriter w;
    CHECK(!write_struct("FRepMovement", Value(m), w), "a rotation component of 300 does not fit byte components");
    m.rotation = {100, 0, 200};
    CHECK(struct_trip("FRepMovement", Value(m)).has_value(), "and 200 does");
}

static void test_quantized_vectors() {
    std::printf("FVector_NetQuantize100 and the quantized forms\n");
    Value v;
    CHECK(captured_round_trip(capture_blocks::kQuantize100Scale, struct_reader("FVector_NetQuantize100"),
                              struct_writer("FVector_NetQuantize100"), &v),
          "PlayerState RelativeScale3D: 31 bits read to the last bit and written back identically");
    const QuantVec* q = v.get_if<QuantVec>();
    CHECK(q && q->mode == QuantVec::Mode::Scaled && q->x == 100 && q->y == 100 && q->z == 100, "(100, 100, 100) scaled");
    CHECK(q && quantize_vector(100, 1, 1, 1) == *q, "quantize_vector(100, 1, 1, 1) is that value");

    QuantVec unscaled{QuantVec::Mode::Unscaled, -5, 1000, 7};
    QuantVec raw_float{QuantVec::Mode::RawFloat, 0x3f800000, 0, 0xc0000000};
    QuantVec raw_double{QuantVec::Mode::RawDouble, 0x3ff0000000000000ll, 0, static_cast<int64_t>(0xc000000000000000ull)};
    for (const QuantVec& x : {unscaled, raw_float, raw_double}) {
        const auto back = struct_trip("FVector_NetQuantize", Value(x));
        const QuantVec* b = back ? back->get_if<QuantVec>() : nullptr;
        CHECK(b && *b == x, x.mode == QuantVec::Mode::Unscaled ? "unscaled integers round trip"
                          : x.mode == QuantVec::Mode::RawFloat ? "a full precision float vector round trips"
                                                                : "a full precision double vector round trips");
    }
    const QuantVec huge = quantize_vector(100, 1e17, 0, 0);   // 1e19 scaled: beyond 2^62
    CHECK(huge.mode == QuantVec::Mode::RawDouble, "a vector too large to scale goes out at full precision");
    CHECK(quantize_vector(10, 0, std::nan(""), 0) == quantize_vector(10, 0, 0, 0), "a NaN vector goes out as zero");
    CHECK((quantize_vector(100, -0.006, 0.004, 0) == QuantVec{QuantVec::Mode::Scaled, -1, 0, 0}), "rounding is half away from zero");
    const Vec3d d = dequantize_vector(QuantVec{QuantVec::Mode::Scaled, 150, -25, 0}, 10);
    CHECK(d.x == 15.0 && d.y == -2.5 && d.z == 0.0, "dequantize divides scaled integers by the scale");
}

static void test_rotator() {
    std::printf("FRotator: PC AttachmentReplication.RotationOffset\n");
    Value v;
    CHECK(captured_round_trip(capture_blocks::kRotatorOffset, struct_reader("FRotator"), struct_writer("FRotator"), &v),
          "35 bits read to the last bit and written back identically");
    const Rot* r = v.get_if<Rot>();
    CHECK(r && r->pitch == 61115 && r->yaw == 17213 && r->roll == 0, "pitch 61115, yaw 17213, roll 0 (0 costs one bit)");
}

static void test_unique_net_id() {
    std::printf("FUniqueNetIdRepl: PlayerState UniqueId\n");
    Value v;
    CHECK(captured_round_trip(capture_blocks::kUniqueNetIdOpen, struct_reader("FUniqueNetIdRepl"), struct_writer("FUniqueNetIdRepl"), &v),
          "464 bits read to the last bit and written back identically");
    const UniqueNetId* id = v.get_if<UniqueNetId>();
    CHECK(id && id->flags == 8 && id->id == "DESKTOP-EXAMPLE1-US-45739BC044607BD9A50196AF41F22492",
          "flags 8 (unencoded, type hash 1) and the id string");

    UniqueNetId encoded;
    encoded.flags = 1 | (2u << 3);
    encoded.bytes = {0xde, 0xad, 0xbe, 0xef};
    UniqueNetId empty;
    empty.flags = 1 | 2;
    UniqueNetId v2;
    v2.flags = 1 | (30u << 3);
    v2.services = 6;
    v2.bytes = {1, 2, 3};
    UniqueNetId other;
    other.flags = 31u << 3;
    other.type = "Steam";
    other.id = "76561198000000000";
    bool all = true;
    for (const UniqueNetId& x : {encoded, empty, v2, other}) {
        const auto back = struct_trip("FUniqueNetIdRepl", Value(x));
        const UniqueNetId* b = back ? back->get_if<UniqueNetId>() : nullptr;
        all = all && b && *b == x;
    }
    CHECK(all, "encoded, empty, V2 and typed forms round trip");
}
// == C O A | Cyb ==

static void test_load_tracker() {
    std::printf("FCharacterLoadTracker: PC CharacterLoadTracker (pkt#46, #72)\n");
    Value a, b;
    CHECK(captured_round_trip(capture_blocks::kLoadTracker46, struct_reader("FCharacterLoadTracker"),
                              struct_writer("FCharacterLoadTracker"), &a),
          "pkt#46: 806 bits read to the last bit and written back identically");
    CHECK(captured_round_trip(capture_blocks::kLoadTracker72, struct_reader("FCharacterLoadTracker"),
                              struct_writer("FCharacterLoadTracker"), &b),
          "pkt#72: 806 bits read to the last bit and written back identically");
    const LoadTracker* t = a.get_if<LoadTracker>();
    const LoadTracker* u = b.get_if<LoadTracker>();
    CHECK(t && t->flags.size() == 25 && t->flags[0] && !t->flags[1] && !t->flags[24], "pkt#46 is 1000...0");
    CHECK(u && u->flags.size() == 25 && u->flags[0] && !u->flags[1] && u->flags[2] && u->flags[24], "pkt#72 is 1011...1");
    LoadTracker too_many;
    too_many.flags.assign(26, true);
    BunchWriter w;
    CHECK(!write_struct("FCharacterLoadTracker", Value(too_many), w), "26 flags cannot be written");
    BunchWriter n26;
    n26.write_bits(26, 6);
    for (int i = 0; i < 26; ++i) n26.write_uint32(0);
    PacketReader r(n26.data(), n26.byte_size(), n26.bit_pos());
    CHECK(!read_struct("FCharacterLoadTracker", r).has_value(), "a count of 26 is refused");
}

static void test_gameplay_tags() {
    std::printf("FGameplayTag: GameState WeaponComboTag; FGameplayTagContainer\n");
    Value v;
    CHECK(captured_round_trip(capture_blocks::kGameplayTagCombat, struct_reader("FGameplayTag"), struct_writer("FGameplayTag"), &v),
          "225 bits read to the last bit and written back identically");
    const GameplayTag* t = v.get_if<GameplayTag>();
    CHECK(t && !t->name.hardcoded && t->name.text == "ability.weaponcombo" && t->name.number == 0, "its FName is ability.weaponcombo");

    GameplayTagContainer two;
    two.tags = {NameValue::plain("ability.sprint"), NameValue::hardcoded_name(3)};
    const auto back = struct_trip("FGameplayTagContainer", Value(two));
    const GameplayTagContainer* c = back ? back->get_if<GameplayTagContainer>() : nullptr;
    CHECK(c && *c == two, "a container of two tags round trips");
    BunchWriter w;
    write_struct("FGameplayTagContainer", Value(GameplayTagContainer{}), w);
    CHECK(w.bit_pos() == 1, "an empty container is one bit");
}

static void test_other_structs() {
    std::printf("FVector2D, FDateTime, FVector_NetQuantizeNormal, unknown structs\n");
    const auto v2 = struct_trip("FVector2D", Value(Vec2d{1.5, -2.25}));
    CHECK((v2 && v2->get_if<Vec2d>() && *v2->get_if<Vec2d>() == Vec2d{1.5, -2.25}), "FVector2D round trips (two doubles)");
    const auto ticks = struct_trip("FDateTime", Value(Ticks{-638000000000000000ll}));
    CHECK(ticks && ticks->get_if<Ticks>() && ticks->get_if<Ticks>()->ticks == -638000000000000000ll, "FDateTime round trips (64 bits)");
    const auto n = struct_trip("FVector_NetQuantizeNormal", Value(Normal16{0, 32768, 65535}));
    CHECK((n && n->get_if<Normal16>() && *n->get_if<Normal16>() == Normal16{0, 32768, 65535}), "FVector_NetQuantizeNormal round trips (3 x 16 bits)");
    BunchWriter w;
    write_struct("FVector_NetQuantizeNormal", Value(Normal16{1, 2, 3}), w);
    CHECK(w.bit_pos() == 48, "and is 48 bits, the width the schema gives it");

    CHECK(!is_known_struct("FRewardInstance") && is_known_struct("FRepMovement"),
          "AoC's FRewardInstance has no codec here");
    BunchWriter zero;
    zero.write_bits(0, 64);
    PacketReader r(zero.data(), zero.byte_size(), zero.bit_pos());
    CHECK(!read_struct("FRewardInstance", r).has_value(), "an unknown struct is not read");
    BunchWriter out;
    CHECK(!write_struct("FRewardInstance", Value(Vec3d{}), out) && out.bit_pos() == 0, "an unknown struct is not written");
    CHECK(!write_struct("FVector", Value(Vec2d{}), out) && out.bit_pos() == 0, "a value of the wrong type is not written");
}

static void test_text() {
    std::printf("FText: GameState AutoFollowFailed / AutoFollowTimedOut\n");
    auto read = [](PacketReader& r) -> std::optional<Value> {
        auto t = read_text(r);
        return t ? std::optional<Value>(Value(*t)) : std::nullopt;
    };
    auto write = [](const Value& v, BunchWriter& w) { return v.get_if<TextValue>() && write_text(w, *v.get_if<TextValue>()); };
    Value a, b;
    CHECK(captured_round_trip(capture_blocks::kTextAutoFollowFailed, read, write, &a), "528 bits read to the last bit and written back identically");
    CHECK(captured_round_trip(capture_blocks::kTextAutoFollowTimedOut, read, write, &b), "488 bits read to the last bit and written back identically");
    const TextValue* t = a.get_if<TextValue>();
    CHECK(t && t->history == 0 && t->flags == 0 && t->ns == "UI" && t->key == "AutoFollowFailed" && t->source == "Failed To Enable Auto-Follow",
          "Base history: namespace UI, key AutoFollowFailed, source string");
// C-O-A captures, Cyb code

    for (bool has : {false, true}) {
        TextValue none;
        none.flags = 2;
        none.history = 0xFF;
        none.has_string = has;
        if (has) none.source = "invariant";
        BunchWriter w;
        write_text(w, none);
        PacketReader r(w.data(), w.byte_size(), w.bit_pos());
        const auto back = read_text(r);
        CHECK(back && *back == none && r.pos() == w.bit_pos(), has ? "None history with a culture-invariant string round trips"
                                                                    : "None history without a string round trips");
    }
    TextValue other;
    other.history = 4;
    BunchWriter w;
    CHECK(!write_text(w, other), "a history type outside None and Base is not written");
}

static void test_soft_object_and_name() {
    std::printf("soft object, FName, FString, object\n");
    Value v;
    CHECK(captured_round_trip(capture_blocks::kSoftObjectCriticalHit,
                              [](PacketReader& r) -> std::optional<Value> { auto s = read_soft_object(r); return s ? std::optional<Value>(Value(*s)) : std::nullopt; },
                              [](const Value& x, BunchWriter& w) { return x.get_if<SoftObjectValue>() && write_soft_object(w, *x.get_if<SoftObjectValue>()); }, &v),
          "soft object: 529 bits read to the last bit and written back identically");
    const SoftObjectValue* s = v.get_if<SoftObjectValue>();
    CHECK(s && s->is_path && s->path == "/Game/WwiseAudio/Events/Main/UI/UI_CriticalHit.UI_CriticalHit", "it is a path string");
    SoftObjectValue by_object;
    by_object.object = emit::FIntrepidNetworkGUID::from_u64(42);
    BunchWriter w;
    write_soft_object(w, by_object);
    PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    const auto back = read_soft_object(r);
    CHECK(back && *back == by_object && w.bit_pos() == 1 + 128, "a soft object naming an object is a bit and the 128-bit GUID");

    Value n;
    CHECK(captured_round_trip(capture_blocks::kNameRemoteParam,
                              [](PacketReader& r2) -> std::optional<Value> { auto x = read_name(r2); return x ? std::optional<Value>(Value(*x)) : std::nullopt; },
                              [](const Value& x, BunchWriter& w2) { return x.get_if<NameValue>() && write_name(w2, *x.get_if<NameValue>()); }, &n),
          "FName: 129 bits read to the last bit and written back identically");
    CHECK(n.get_if<NameValue>() && n.get_if<NameValue>()->text == "bRemote" && n.get_if<NameValue>()->number == 0, "bRemote, no number");
    BunchWriter hw;
    write_name(hw, NameValue::hardcoded_name(0));
    CHECK(hw.bit_pos() == 9, "a hardcoded name is a bit and one SIP byte (EName None = 0)");
    BunchWriter nw;
    write_name(nw, NameValue::plain("Slot", 3));
    PacketReader nr(nw.data(), nw.byte_size(), nw.bit_pos());
    const auto nb = read_name(nr);
    CHECK(nb && *nb == NameValue::plain("Slot", 3), "a numbered name round trips");

    Value str;
    CHECK(captured_round_trip(capture_blocks::kStringPlayerName,
                              [](PacketReader& r2) -> std::optional<Value> { auto x = read_fstring(r2); return x ? std::optional<Value>(Value::str(*x)) : std::nullopt; },
                              [](const Value& x, BunchWriter& w2) { return x.get_if<std::string>() && write_fstring(w2, *x.get_if<std::string>()); }, &str),
          "FString: 200 bits read to the last bit and written back identically");
    CHECK(str.get_if<std::string>() && *str.get_if<std::string>() == "DESKTOP-EXAMPLE1-US-", "the player name");

    BunchWriter oe;
    CHECK(!write_object(oe, emit::FIntrepidNetworkGUID::from_u64(1)), "ObjectId 1 (the export form) is not written as a property value");
}

static void test_strings() {
    std::printf("FString: ASCII, UCS-2, empty, damaged\n");
    struct Case { const char* text; int32_t length; };
    const Case cases[] = {
        {"", 0},
        {"abc", 4},
        {"Caf\xc3\xa9", -5},                       // "Café": not pure ASCII, sent as UCS-2
        {"\xe6\x97\xa5\xe6\x9c\xac", -3},          // two BMP characters
        {"\xf0\x9f\x98\x80!", -4},                 // a surrogate pair and an ASCII character
    };
    bool all = true;
    for (const Case& c : cases) {
        BunchWriter w;
        write_fstring(w, c.text);
        PacketReader head(w.data(), w.byte_size(), w.bit_pos());
        const int32_t len = static_cast<int32_t>(head.read_uint32());
        PacketReader body(w.data(), w.byte_size(), w.bit_pos());
        const auto s = read_fstring(body);
        all = all && len == c.length && s && *s == c.text && body.pos() == w.bit_pos();
    }
    CHECK(all, "ANSI when pure ASCII (positive length), UTF-16 code units otherwise (negative), 0 for empty; all read back");
    BunchWriter w;
    write_fstring(w, "hello");
    PacketReader cut(w.data(), w.byte_size(), w.bit_pos() - 8);
    CHECK(!read_fstring(cut).has_value(), "a string cut short is not read");
    BunchWriter bad;
    CHECK(!write_fstring(bad, std::string("\xff\xfe")), "invalid UTF-8 is not written");
}
// C O A + Cyb

static void test_string_limits() {
    std::printf("FString: the limits the writer, the reader and fstring_fits share\n");
    const std::string longest(kMaxFStringChars, 'x');
    CHECK(fstring_fits(longest) && fstring_fits("") && fstring_fits("Caf\xc3\xa9"), "65,535 characters, empty and non-ASCII strings fit");
    CHECK(!fstring_fits(longest + "x") && !fstring_fits(std::string("a\0b", 3)) && !fstring_fits("\xff"),
          "65,536 characters, a NUL and invalid UTF-8 do not");
    BunchWriter w;
    CHECK(write_fstring(w, longest), "the longest string is written");
    PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    const auto back = read_fstring(r);
    CHECK(back && *back == longest && r.pos() == w.bit_pos(), "and read back: the length on the wire is 65,536 with the NUL");
    // The reader refuses one more, whatever the writer is: a 65,537-long wire string
    BunchWriter over;
    over.write_int32(static_cast<int32_t>(kMaxFStringChars) + 2);
    for (size_t i = 0; i <= kMaxFStringChars; ++i) over.write_uint8('x');
    over.write_uint8(0);
    PacketReader ro(over.data(), over.byte_size(), over.bit_pos());
    CHECK(!read_fstring(ro).has_value(), "a wire length of 65,537 is refused");
    BunchWriter nul;
    nul.write_int32(4);
    nul.write_uint8('a');
    nul.write_uint8(0);
    nul.write_uint8('b');
    nul.write_uint8(0);
    PacketReader rn(nul.data(), nul.byte_size(), nul.bit_pos());
    CHECK(!read_fstring(rn).has_value(), "an embedded NUL on the wire is refused (so the writer does not write one)");
    CHECK(name_fits(NameValue::plain("ok")) && !name_fits(NameValue::plain(std::string("a\0", 2))) &&
              name_fits(NameValue::hardcoded_name(3)),
          "name_fits: a plain name's text must fit, a hardcoded name has none");
}

static void test_quantization_levels() {
    std::printf("FRepMovement's location and velocity quantization levels\n");
    const CodecOptions def;
    CHECK(def.rep_movement_location_scale == 100 && def.rep_movement_velocity_scale == 1,
          "the defaults are the captured pawn's: location to two decimals (APawn), velocity to whole numbers");
    Value v;
    captured_round_trip(capture_blocks::kRepMovementOpen, struct_reader("FRepMovement"), struct_writer("FRepMovement"), &v);
    const RepMovement* m = v.get_if<RepMovement>();
    if (m) {
        const Vec3d p = dequantize_location(def, m->location);
        CHECK(std::fabs(p.x - -777664.5) < 1e-6 && quantize_location(def, p.x, p.y, p.z) == m->location,
              "the captured location reads as (-777664.5, ...) and quantizes back to its integers");
    } else {
        CHECK(false, "the captured FRepMovement reads");
    }
    CodecOptions whole;   // an actor that is not a Pawn: AActor's FRepMovement default is a whole number
    whole.rep_movement_location_scale = 1;
    const QuantVec at2 = quantize_location(def, 1234.5, -20.25, 3.0);
    const QuantVec at0 = quantize_location(whole, 1234.5, -20.25, 3.0);
    CHECK(at2 == (QuantVec{QuantVec::Mode::Scaled, 123450, -2025, 300}) && at0 == (QuantVec{QuantVec::Mode::Scaled, 1235, -20, 3}),
          "the same location is 100 times the integers at two decimals (the reader's scale is part of the value)");
    const Vec3d back = dequantize_location(whole, at0);
    CHECK(back == (Vec3d{1235, -20, 3}), "and dequantizes with the level it was made with");
    CodecOptions tenths;
    tenths.rep_movement_velocity_scale = 10;
    CHECK(quantize_velocity(def, 600, 0, -980) == quantize_vector(1, 600, 0, -980) &&
              quantize_velocity(tenths, 60.5, 0, -9.8) == (QuantVec{QuantVec::Mode::Scaled, 605, 0, -98}) &&
              dequantize_velocity(tenths, QuantVec{QuantVec::Mode::Scaled, 605, 0, -98}) == (Vec3d{60.5, 0, -9.8}),
          "the velocity level is its own: whole numbers by default, tenths when the object says so");
    RepMovement moved;
    moved.location = at0;
    moved.linear_velocity = quantize_velocity(tenths, 60.5, 0, -9.8);
    const auto trip = struct_trip("FRepMovement", Value(moved));
    CHECK(trip && trip->get_if<RepMovement>() && *trip->get_if<RepMovement>() == moved, "an FRepMovement made with other levels writes and reads back");
}

static void test_zero_structs() {
    std::printf("the zero value of a struct\n");
    const char* known[] = {"FVector", "FVector3d", "FVector2D", "FDateTime", "FTimespan", "FRotator", "FVector_NetQuantize",
                           "FVector_NetQuantize100", "FGameplayTag", "FGameplayTagContainer"};
    bool all = true;
    for (const char* name : known) {
        const auto z = zero_struct(name);
        all = all && z && struct_accepts(name, *z) && struct_trip(name, *z) == z;
    }
    CHECK(all, "FVector, FVector2D, ticks, FRotator, quantized vectors and the tags have one; it is accepted and round trips");
    const auto v = zero_struct("FVector");
    CHECK(v && *v == Value(Vec3d{}), "a zeroed FVector is (0, 0, 0)");
    CHECK(!zero_struct("FRepMovement") && !zero_struct("FAppearanceId") && !zero_struct("FUniqueNetIdRepl") && !zero_struct("FNoSuchStruct"),
          "a struct whose zero is not known, and an unknown struct, have none");
}

int main() {
    test_fvector();
    test_rep_movement();
    test_rep_movement_forms();
    test_quantized_vectors();
    test_rotator();
    test_unique_net_id();
    test_load_tracker();
    test_gameplay_tags();
    test_other_structs();
    test_text();
    test_soft_object_and_name();
    test_strings();
    test_string_limits();
    test_quantization_levels();
    test_zero_structs();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

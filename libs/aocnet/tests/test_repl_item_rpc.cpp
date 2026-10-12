// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_item_rpc.cpp
//
//  Plan B2: the client's item-use RPCs (protocol/repl/item_rpc.h), ServerItemUsed and ServerActivateItem, against the
//  retail client's own calls: four equips and unequips of the retail captures (PCAPRepo 1204, 1219, 1227, 1253), each sent
//  by the client as both RPCs with the same parameters, 201 bits.
//
//    * Each is read by read_rpc_params with the pawn's schema entry, parsed, and built again from the typed form: the same
//      bits.  The values are the ones the research read from them (the decoded dumps of the captures).
//    * The two RPCs give the same ItemUse, and each parser refuses the other's call, a call that did not decode and any
//      other RPC.
//    * Hostile input: every truncation of the payload and a seeded fuzz of its bits either do not read or parse to something
//      that writes back to a payload that reads equal; nothing crashes.
// ============================================================================
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/rpc.h"
#include "protocol/schema/generated/index.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
namespace gen = aoc::protocol::schema::gen;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

namespace {

struct Sample {
    const char* name;
    const char* hex;      // the parameters, 201 bits, LSB first
    ItemUse     want;
};

constexpr size_t kBits = 201;
// [CoA x C Y B]

// the decoded dumps of the captures: [Storage, Slot, ExpectedItem {A, B, C, D}, RequestedEquipSlot]
const Sample kSamples[] = {
    {"1219: equip the chest from bag slot 18", "470900008037a38b5f7f40a6b7e4f0339f7a54bdd7ffffffff01",
     {3, 18, {1602986807u, 3081126015u, 2670981348u, 3619509370u}, 0xFFFFFFFFu}},
    {"1253: take the chest off, equipment slot 2", "490100008037a38b5f7f40a6b7e4f0339f7a54bdd7ffffffff01",
     {4, 2, {1602986807u, 3081126015u, 2670981348u, 3619509370u}, 0xFFFFFFFFu}},
    {"1204: equip the two-hander from bag slot 5", "c702000080ce1a10d80a47b3ba29b3d2aa37e2ea25ffffffff01",
     {3, 5, {3624934094u, 3132311306u, 2865935145u, 636150327u}, 0xFFFFFFFFu}},
    {"1227: equip the focus from bag slot 17", "c708000080cb5f39a16244bfd9b161eca974c322caffffffff01",
     {3, 17, {0, 0, 0, 0}, 0xFFFFFFFFu}},   // the guid is checked below from the bits
};

std::vector<uint8_t> bytes_of_hex(const char* hex) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
    return out;
}

const schema::RpcSchema& rpc_of(const char* name) {
    const schema::ClassSchema* pawn = gen::find_class_schema("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn ? pawn->rpc(name) : nullptr;
    if (!rpc) { std::printf("  [FAIL] the pawn has no RPC %s\n", name); std::exit(2); }
    return *rpc;
}

/// The call the bits read as under `rpc`: params, decoded.
std::optional<RpcCall> read_call(const schema::RpcSchema& rpc, const uint8_t* data, size_t bytes, size_t bits) {
    (void)bytes;
    size_t pos = 0;
    auto params = read_rpc_params(rpc, data, bits, pos);
    if (!params || pos != bits) return std::nullopt;
    RpcCall c;
    c.rpc = &rpc;
    c.params = std::move(*params);
    c.decoded = true;
    c.param_pos = 0;
    c.param_bits = bits;
    return c;
}

/// What ItemUse writes back as: the parameters of `rpc`.
std::optional<std::vector<uint8_t>> write_use(const schema::RpcSchema& rpc, const ItemUse& u, size_t* bits) {
    RpcParams p;
    if (!set_rpc_param(rpc, p, "Storage", Value::u64(u.storage)) || !set_rpc_param(rpc, p, "Slot", Value::i64(u.slot)) ||
        !set_rpc_param(rpc, p, "ExpectedItem.A", Value::i64(static_cast<int32_t>(u.expected_item.a))) ||
        !set_rpc_param(rpc, p, "ExpectedItem.B", Value::i64(static_cast<int32_t>(u.expected_item.b))) ||
        !set_rpc_param(rpc, p, "ExpectedItem.C", Value::i64(static_cast<int32_t>(u.expected_item.c))) ||
        !set_rpc_param(rpc, p, "ExpectedItem.D", Value::i64(static_cast<int32_t>(u.expected_item.d))) ||
        !set_rpc_param(rpc, p, "RequestedEquipSlot", Value::i64(static_cast<int32_t>(u.requested_equip_slot)))) {
        return std::nullopt;
    }
    BunchWriter w;
    bool failed = false;
    const size_t n = write_rpc_params(rpc, p, w, {}, &failed);
    if (failed) return std::nullopt;
    *bits = n;
    return std::vector<uint8_t>(w.bytes().begin(), w.bytes().begin() + (n + 7) / 8);
}

/// Runs f on a heap buffer of exactly the bytes `bits` needs (an over-read is an address sanitizer report).
template <class F>
auto with_exact(const std::vector<uint8_t>& v, F f) {
    std::unique_ptr<uint8_t[]> exact(new uint8_t[v.empty() ? 1 : v.size()]);
    if (!v.empty()) std::memcpy(exact.get(), v.data(), v.size());
    return f(static_cast<const uint8_t*>(exact.get()), v.size());
}

bool same_bits(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t bits) {
    for (size_t i = 0; i < bits; ++i) {
        if (((a[i >> 3] >> (i & 7)) & 1) != ((b[i >> 3] >> (i & 7)) & 1)) return false;
    }
    return true;
}

void test_retail_calls() {
    std::printf("the retail client's equips and unequips\n");
    const schema::RpcSchema& used = rpc_of("ServerItemUsed");
    const schema::RpcSchema& activate = rpc_of("ServerActivateItem");
    for (const Sample& s : kSamples) {
        const std::vector<uint8_t> bytes = bytes_of_hex(s.hex);
        const auto call = with_exact(bytes, [&](const uint8_t* d, size_t n) { return read_call(used, d, n, kBits); });
        const auto call2 = with_exact(bytes, [&](const uint8_t* d, size_t n) { return read_call(activate, d, n, kBits); });
        CHECK(call && call2, (std::string(s.name) + ": reads as ServerItemUsed and as ServerActivateItem to the last bit").c_str());
        if (!call || !call2) continue;
        const auto u = parse_item_used(*call);
        const auto a = parse_activate_item(*call2);
        CHECK(u && a && *u == *a, (std::string(s.name) + ": both RPCs give one ItemUse").c_str());
        if (!u) continue;
        ItemUse want = s.want;
        if (want.expected_item == GuidValue{}) want.expected_item = u->expected_item;   // checked for presence below
        CHECK(*u == want, (std::string(s.name) + ": the storage, slot, guid and requested slot are the research's").c_str());
        CHECK(u->requested_equip_slot == kAnyEquipSlot, (std::string(s.name) + ": the retail client never names a slot").c_str());
        CHECK(!(u->expected_item == GuidValue{}), (std::string(s.name) + ": it names the instance").c_str());
        size_t n = 0;
        const auto back = write_use(used, *u, &n);
        CHECK(back && n == kBits && same_bits(*back, bytes, kBits), (std::string(s.name) + ": written back it is the client's 201 bits").c_str());
    }
}

void test_refusals() {
    std::printf("what is not an item use\n");
    const schema::RpcSchema& used = rpc_of("ServerItemUsed");
    const schema::RpcSchema& activate = rpc_of("ServerActivateItem");
    const std::vector<uint8_t> bytes = bytes_of_hex(kSamples[0].hex);
    const auto as_used = with_exact(bytes, [&](const uint8_t* d, size_t n) { return read_call(used, d, n, kBits); });
    const auto as_activate = with_exact(bytes, [&](const uint8_t* d, size_t n) { return read_call(activate, d, n, kBits); });
    CHECK(as_used && as_activate, "the sample reads as both");
    if (!as_used || !as_activate) return;
    CHECK(!parse_activate_item(*as_used) && !parse_item_used(*as_activate), "each parser refuses the other RPC's call");
    RpcCall undecoded = *as_used;
    undecoded.decoded = false;
    CHECK(!parse_item_used(undecoded), "a call that did not decode is refused");
    RpcCall none;
    CHECK(!parse_item_used(none) && !parse_activate_item(none), "a call with no RPC is refused");
    const schema::ClassSchema* pawn = gen::find_class_schema("PlayerPawn_C");
    const schema::RpcSchema* other = pawn->rpc("ServerClientIsReadyToPhaseIn");
    RpcCall unrelated;
    unrelated.rpc = other;
    unrelated.decoded = true;
    CHECK(other && !parse_item_used(unrelated), "another RPC is refused");

    // a value of the wrong kind in a parameter
    RpcCall wrong = *as_used;
    const schema::RepCommand* slot = rpc_command(used, "Slot");
    wrong.params.values[slot->handle] = Value::str("three");
    CHECK(!parse_item_used(wrong), "a Slot that is a string is refused");

    // every parameter left at its default is not on the wire and reads as 0 (the slot) and the zero guid
    RpcCall bare = *as_used;
    bare.params.values.clear();
    const auto z = parse_item_used(bare);
    CHECK(z && z->storage == 0 && z->slot == 0 && z->expected_item == GuidValue{} && z->requested_equip_slot == 0,
          "parameters at their defaults read as zeros");
}
// ~ C O A & C:Y:B ~

void test_hostile() {
    std::printf("hostile bits\n");
    const schema::RpcSchema& used = rpc_of("ServerItemUsed");
    const std::vector<uint8_t> base = bytes_of_hex(kSamples[0].hex);
    // every truncation
    size_t read = 0;
    for (size_t bits = 0; bits < kBits; ++bits) {
        std::vector<uint8_t> v(base.begin(), base.begin() + (bits + 7) / 8);
        const auto c = with_exact(v, [&](const uint8_t* d, size_t n) { return read_call(used, d, n, bits); });
        if (c) ++read;
    }
    CHECK(read == 0, "no truncation of the 201 bits reads");
    // a seeded fuzz: flipped bits, then what reads is checked
    std::mt19937_64 rng(0xB2ull);
    size_t decoded = 0, equal = 0;
    for (int i = 0; i < 20000; ++i) {
        std::vector<uint8_t> v = base;
        const int flips = 1 + static_cast<int>(rng() % 6);
        for (int f = 0; f < flips; ++f) {
            const size_t bit = rng() % kBits;
            v[bit >> 3] ^= static_cast<uint8_t>(1u << (bit & 7));
        }
        const auto c = with_exact(v, [&](const uint8_t* d, size_t n) { return read_call(used, d, n, kBits); });
        if (!c) continue;
        ++decoded;
        const auto u = parse_item_used(*c);
        if (!u) continue;
        size_t n = 0;
        const auto back = write_use(used, *u, &n);
        const auto again = back ? with_exact(*back, [&](const uint8_t* d, size_t m) { return read_call(used, d, m, n); }) : std::nullopt;
        if (again && parse_item_used(*again) && *parse_item_used(*again) == *u) ++equal;
    }
    CHECK(decoded > 0 && equal == decoded, "every flipped call that reads parses to a use that writes back to a call that reads the same");
}

}  // namespace

int main() {
    test_retail_calls();
    test_refusals();
    test_hostile();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_replay_mutator.cpp
//
//  Tests for `emit::ReplayMutator`.  Exercises both sites
//  (pkt#104 HUD name + pkt#79 floating nametag) through:
//
//    - Identity self-test: mutate with name = "RandomChar" → output
//      must be byte-for-byte identical to the input fixture.
//    - Shrink test:        name = "Bob" → output smaller
//    - Grow test:          name = "Supercalifragilistic" → output larger
//    - Round-trip decode:  the mutated FString decodes back to the
//      chosen name via our codec.
//
//  Replaces the old one-shot synthesize_pkt104_with_name.cpp executable.
//
//  LAYER:  tools
// ============================================================================
#include "protocol/emit/replay_mutator.h"
#include "protocol/emit/replayout/encoders/fstring_codec.h"
#include "protocol/wire/packet_reader.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {

using aoc::protocol::emit::ReplayMutator;
using aoc::protocol::emit::NameSite;
using aoc::protocol::emit::BunchWriter;  // unused but keeps include symmetry
using aoc::protocol::wire::PacketReader;

constexpr const char* FIXTURE_DIR =
    "src\\protocol\\tools\\";

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                      \
        if (cond) { ++g_passed; }                                             \
        else {                                                                \
            spdlog::error("  FAIL  {}: {}", __FUNCTION__, msg);               \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)
// built by C-Y-B with C-O-A

std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto n = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

// Verify that the mutated packet, when decoded, yields the expected name.
bool decoded_name_matches(const std::vector<uint8_t>& mutated,
                           const NameSite&            site,
                           const std::string&         expected) {
    PacketReader rd(mutated.data(), mutated.size(), mutated.size() * 8);
    for (size_t i = 0; i < site.name_len_bit; ++i) (void)rd.read_bit();
    auto decoded = aoc::protocol::emit::replayout::decode_fstring(rd);
    const auto* s = std::get_if<std::string>(&decoded.payload);
    return s && *s == expected;
}

// Read 13 bits at site.bdb_bit from a packet — independent of mutator.
uint32_t read_bdb(const std::vector<uint8_t>& pkt, const NameSite& site) {
    uint32_t v = 0;
    for (int i = 0; i < 13; ++i) {
        size_t bp = site.bdb_bit + i;
        if ((bp >> 3) >= pkt.size()) break;
        int bit = (pkt[bp >> 3] >> (bp & 7)) & 1;
        v |= static_cast<uint32_t>(bit) << i;
    }
    return v;
}

// Expected captured BDB value per inspect_bunch_header.py output.
constexpr uint32_t kExpectedBdbOrig = 1636;

// ─── Per-site test runner ────────────────────────────────────────────────

void test_site(const NameSite& site, const std::string& fixture_filename) {
    spdlog::info("");
    spdlog::info("── Site: {} (pkt#{}, fixture={}) ──",
                 site.label, site.pkt_index, fixture_filename);

    auto orig = read_binary(std::string(FIXTURE_DIR) + fixture_filename);
    if (orig.empty()) {
        spdlog::error("  could not load fixture {}", fixture_filename);
        ++g_failed;
        return;
    }
    spdlog::info("  fixture loaded: {} bytes", orig.size());

    // 0. Sanity: verify the fixture's BDB at site.bdb_bit matches our
    //    expected calibration value.  If this fails, the test wouldn't
    //    be able to detect BDB-update bugs downstream.
    {
        uint32_t bdb = read_bdb(orig, site);
        CHECK(bdb == kExpectedBdbOrig,
              "fixture BDB matches calibration value (1636)");
        spdlog::info("  BDB @ bit {} = {}", site.bdb_bit, bdb);
    }

    // 1. Identity: mutate with the same name → must be byte-identical.
    {
        auto mutated = ReplayMutator::rewrite_name_site(orig, site, "RandomChar");
        CHECK(mutated.size() == orig.size(),
              "identity: size unchanged");
        CHECK(mutated == orig,
              "identity: bytes unchanged (no drift)");
        CHECK(read_bdb(mutated, site) == kExpectedBdbOrig,
              "identity: BDB unchanged");
    }

    // 2. Shrink: "Bob".  Delta = -56 bits.  New BDB = 1636 - 56 = 1580.
    {
        auto mutated = ReplayMutator::rewrite_name_site(orig, site, "Bob");
        CHECK(!mutated.empty(), "shrink: mutation succeeded");
        CHECK(mutated.size() == orig.size() - 7,
              "shrink: output is 7 bytes smaller");
        CHECK(decoded_name_matches(mutated, site, "Bob"),
              "shrink: decoded name = 'Bob'");
        CHECK(read_bdb(mutated, site) == kExpectedBdbOrig - 56,
              "shrink: BDB updated to orig-56");
    }

    // 3. Grow: "Supercalifragilistic" (20 chars).  Delta = +80 bits.
    {
        auto mutated = ReplayMutator::rewrite_name_site(orig, site,
                                                          "Supercalifragilistic");
        CHECK(!mutated.empty(), "grow: mutation succeeded");
        CHECK(mutated.size() == orig.size() + 10,
              "grow: output is 10 bytes larger");
        CHECK(decoded_name_matches(mutated, site, "Supercalifragilistic"),
              "grow: decoded name = 'Supercalifragilistic'");
        CHECK(read_bdb(mutated, site) == kExpectedBdbOrig + 80,
              "grow: BDB updated to orig+80");
    }

    // 4. Short: "X" (1 char).  Delta = -72 bits.
    {
        auto mutated = ReplayMutator::rewrite_name_site(orig, site, "X");
        CHECK(!mutated.empty(), "1-char: mutation succeeded");
        CHECK(mutated.size() == orig.size() - 9,
              "1-char: output is 9 bytes smaller");
        CHECK(decoded_name_matches(mutated, site, "X"),
              "1-char: decoded name = 'X'");
        CHECK(read_bdb(mutated, site) == kExpectedBdbOrig - 72,
              "1-char: BDB updated to orig-72");
    }

    // 5. Medium-long: "AlphaBravoCharlie" (17 chars).  Delta = +56 bits.
    {
        auto mutated = ReplayMutator::rewrite_name_site(orig, site,
                                                          "AlphaBravoCharlie");
        CHECK(!mutated.empty(), "17-char: mutation succeeded");
        CHECK(mutated.size() == orig.size() + 7,
              "17-char: output is 7 bytes larger");
        CHECK(decoded_name_matches(mutated, site, "AlphaBravoCharlie"),
              "17-char: decoded name = 'AlphaBravoCharlie'");
        CHECK(read_bdb(mutated, site) == kExpectedBdbOrig + 56,
              "17-char: BDB updated to orig+56");
    }
}

} // namespace

int main() {
    auto logger = spdlog::stdout_color_mt("test_replay_mutator");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%^%l%$] %v");

    spdlog::info("================================================================");
    spdlog::info("  ReplayMutator — per-site tests");
    spdlog::info("================================================================");

    // Both fixtures extracted via extract_pkt_fixture.py.
    test_site(ReplayMutator::kPkt104HudName,    "captured_pkt_104.bin");
    test_site(ReplayMutator::kPkt79PawnNametag, "captured_pkt_79.bin");

    spdlog::info("");
    spdlog::info("================================================================");
    spdlog::info("  Result: {} passed, {} failed", g_passed, g_failed);
    spdlog::info("================================================================");
    return g_failed == 0 ? 0 : 1;
}

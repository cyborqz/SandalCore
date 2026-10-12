// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_pc_bootstrap.cpp
//
//  ClientSetHUD as the original server sent it right after opening the
//  PlayerController (replay_data.bin pkt#22, ch=3):
//    chSeq 956  package-map export of BP_AOCHUD_C (873 bits, exports only)
//    chSeq 957  the ClientSetHUD(BP_AOCHUD_C) content block (173 bits)
//  Without it the client has no HUD ("Failed to get AHudBase").
// ============================================================================
#include "protocol/emit/package_map_exporter.h"
#include "protocol/wire/possession.h"
#include <cstdio>
#include <string>

using namespace aoc::protocol::emit;
using namespace aoc::protocol::wire;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static std::string hex_of(const uint8_t* data, size_t bits) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < (bits + 7) / 8; ++i) {
        uint8_t b = data[i];
        if (i == bits / 8 && bits % 8) b &= static_cast<uint8_t>((1u << (bits % 8)) - 1);
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

static void test_hud_class_export() {
    std::printf("BP_AOCHUD_C export (pkt#22 ch=3 chSeq 956)\n");
    ExportEntry package = ExportEntry::asset(7805116325763694055ULL, "/Game/UI/Widgets/BP_AOCHUD");
    package.has_checksum = true;   // flagged with a zero checksum, as the server sent it
    ExportEntry hud = ExportEntry::asset(8463636446363390223ULL, "BP_AOCHUD_C", 0xac6f1251);
    hud.outer = std::make_unique<ExportEntry>(std::move(package));
    std::vector<ExportEntry> entries;
    entries.push_back(std::move(hud));
// (C Y B | CoA)

    BunchWriter w;
    const size_t bits = PackageMapExporter::write_export_section(w, entries);
    CHECK(bits == 873, "export section is 873 bits");
    CHECK(hex_of(w.data(), bits) ==
          "020000001e2214eb02c1e9ea00000000000000000ace7b7ed7e3afa2d800000000000000000a0000000000"
          "0000000000000000000000360000005e8ec2daca5eaa925eaed2c8cecae8e65e84a0be829e8690aa880000"
          "0000001800000084a0be829e8690aa88be8600a224de5801",
          "bits match the original server's export");
}

static void test_client_set_hud_block() {
    std::printf("ClientSetHUD content block (pkt#22 ch=3 chSeq 957)\n");
    CHECK(kPcFieldIdx_ClientSetHUD == 53, "ClientSetHUD = 53");
    FIntrepidNetworkGUID hud_class;
    hud_class.ObjectId = 8463636446363390223ULL;   // static class GUID: ServerId and Randomizer 0

    uint8_t buf[64] = {};
    size_t off = 0;
    const size_t bits = write_pc_object_rpc(buf, sizeof(buf), off, kPcFieldIdx_ClientSetHUD, hud_class);
    CHECK(bits == 173, "content block is 173 bits");
    CHECK(hex_of(buf, bits) == "de08d43020f02142b12e109cae0e0000000000000000",
          "bits match the original server's block");
}

int main() {
    test_hud_class_export();
    test_client_set_hud_block();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

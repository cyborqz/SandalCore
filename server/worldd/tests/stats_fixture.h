// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/stats_fixture.h
//
//  The shared stat corpus (stats/testdata/corpus.txt, written by services/internal/statprog) as worldd's tests need it (plan B3):
//    - the bundle "main", the real stat program (format version 2: the stat graph and the ability expressions), which the content.db fixture stores in stat_program, so a PlayerWorld on
//      the fixture runs the real formulas;
//    - the cases made from gear (the starting kits, the captured Bard and the retail Ranger): the class and level, the worn items
//      (record, quality, slot name), the mods and tags Go made of them, and the digest of every stat the engine gave, so that what
//      worldd computes from the items can be compared with Go's.
//  STATS_CORPUS_PATH is defined by CMake for every worldd test.
// ============================================================================
#pragma once

#include "stats/stat_engine.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef STATS_CORPUS_PATH
#define STATS_CORPUS_PATH "server/worldd/stats/testdata/corpus.txt"
#endif

namespace stats_fixture {

struct CorpusItem {
    int64_t     record = 0;
    int         quality = 1;
    std::string slot;   // an EEquipSlot name ("Chest"), or "Slot<N>" for a value the capture cases give without a name
};

struct CorpusCase {
    std::string                                 name, bundle;
    int                                         class_enum = 0, level = 0;
    std::vector<std::pair<int64_t, uint64_t>>   mods;   // stat guid, the double's bits
    std::vector<std::pair<std::string, std::string>> tags;   // slot name, tag
    std::vector<CorpusItem>                     items;
    bool                                        has_gear = false;
    bool                                        half_up = false;
    std::vector<std::pair<int64_t, uint64_t>>   want;   // stat guid, the double's bits (only some cases list them)
    uint64_t                                    digest = 0;
    size_t                                      members = 0;
    int                                         faults = 0, ignored = 0;
};

struct Corpus {
    std::map<std::string, std::vector<uint8_t>> bundles;
    std::vector<CorpusCase>                     cases;
    const CorpusCase* find(const std::string& name) const {
        for (const CorpusCase& c : cases) {
            if (c.name == name) return &c;
        }
        return nullptr;
    }
};
// -- CoA & C_Y_B --

inline std::vector<uint8_t> base64_decode(const std::string& s) {
    static int8_t t[256];
    static bool init = false;
    if (!init) {
        std::memset(t, -1, sizeof(t));
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        init = true;
    }
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : s) {
        if (c == '=') break;
        const int8_t v = t[static_cast<uint8_t>(c)];
        if (v < 0) return {};
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

/// The corpus, read once; exits the test (status 2) when it cannot be read.
inline const Corpus& corpus() {
    static const Corpus c = [] {
        Corpus out;
        std::ifstream f(STATS_CORPUS_PATH, std::ios::binary);
        if (!f) {
            std::printf("  [FAIL] test setup: cannot open the stat corpus %s\n", STATS_CORPUS_PATH);
            std::exit(2);
        }
        std::string line, b64, bundle_name;
        size_t bundle_len = 0;
        CorpusCase* cur = nullptr;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            std::istringstream is(line);
            std::vector<std::string> w;
            for (std::string x; is >> x;) w.push_back(x);
            if (!bundle_name.empty()) {
                if (w[0] == "endbundle") {
                    std::vector<uint8_t> raw = base64_decode(b64);
                    if (raw.size() != bundle_len) {
                        std::printf("  [FAIL] test setup: the corpus bundle %s has the wrong length\n", bundle_name.c_str());
                        std::exit(2);
                    }
                    out.bundles[bundle_name] = std::move(raw);
                    bundle_name.clear();
                } else {
                    b64 += w[0];
                }
                continue;
            }
            const std::string& k = w[0];
            if (k == "bundle") {
                bundle_name = w[1];
                bundle_len = std::stoul(w[2]);
                b64.clear();
            } else if (k == "case") {
                out.cases.emplace_back();
                cur = &out.cases.back();
                cur->name = w[1];
                cur->bundle = w[2];
                cur->class_enum = std::stoi(w[3]);
                cur->level = std::stoi(w[4]);
            } else if (k == "end") {
                cur = nullptr;
            } else if (cur) {
                if (k == "mod") cur->mods.push_back({std::stoll(w[1]), std::stoull(w[2], nullptr, 16)});
                else if (k == "tag") cur->tags.push_back({w[1], w[2]});
                else if (k == "rounding") {
                    cur->has_gear = true;
                    cur->half_up = w[1] == "halfup";
                } else if (k == "item") {
                    cur->has_gear = true;
                    cur->items.push_back({std::stoll(w[1]), std::stoi(w[2]), w[3]});
                } else if (k == "want") cur->want.push_back({std::stoll(w[1]), std::stoull(w[2], nullptr, 16)});
                else if (k == "digest") {
                    cur->digest = std::stoull(w[1], nullptr, 16);
                    cur->members = std::stoul(w[2]);
                    cur->faults = std::stoi(w[3]);
                    cur->ignored = std::stoi(w[4]);
                }
            }
        }
        if (!out.bundles.count("main")) {
            std::printf("  [FAIL] test setup: the corpus has no bundle main\n");
            std::exit(2);
        }
        return out;
    }();
    return c;
}

/// The FNV-1a 64 of a run's output (every stat's guid, value, base, equipment and regeneration, in stat order): statprog.Digest.
inline uint64_t digest(const aoc::worldd::stats::StatResult& r) {
    uint64_t h = 14695981039346656037ull;
    const auto put = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    const auto u64 = [&](uint64_t v) {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
        put(b, 8);
    };
    const auto bits = [](double d) {
        uint64_t b;
        std::memcpy(&b, &d, 8);
        return b;
    };
    for (const aoc::worldd::stats::StatOut& s : r.stats) {
        u64(static_cast<uint64_t>(s.guid));
        u64(bits(s.value));
        u64(bits(s.base));
        u64(bits(s.equip));
        if (s.has_regen) {
            const uint8_t one = 1;
            put(&one, 1);
            u64(bits(s.regen));
        } else {
            const uint8_t zero = 0;
            put(&zero, 1);
        }
    }
    return h;
}

/// The real stat program: what a content.db's stat_program row holds.
inline const std::vector<uint8_t>& main_bundle() { return corpus().bundles.at("main"); }

}  // namespace stats_fixture

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/tools/replay_property_patcher.h
//
//  Tier-1 fixed-width property patcher for replay packets.
//
//  Rewrites captured replay bytes in-place with user-specified values before
//  the server streams them to the client.  Since all target properties are
//  fixed-width (int32 / float / uint8), no bit-shifting, size-prefix, or
//  partial-bunch reassembly changes are needed — the client cannot detect
//  the modification.
//
//  Each patch descriptor provides:
//    - A "needle" byte pattern that uniquely identifies the property's
//      location inside the captured bunch (typically the captured value
//      preceded/followed by stable context bytes).
//    - The new byte pattern to write at the same offset.
//    - An expected match count (for sanity — property should appear N times).
//
//  USAGE:
//      ReplayPropertyPatcher patcher;
//      patcher.add_int32("character_level", {0x01, 0x00, 0x00, 0x00}, 99);
//      patcher.add_float ("character_hp",   {0x00, 0x00, 0x80, 0x3F}, 9999.0f);
//      patcher.apply_all(replay_data_->packets);
//
//  The apply_all() method returns a summary of what was patched (counts per
//  property) for logging / verification.
//
//  DISCOVERY WORKFLOW:
//  Adding a new property requires knowing:
//    (a) its captured value in the replay (e.g. captured character = level 1)
//    (b) a distinguishing byte pattern in the packet bytes
//  Run the `replay_dump_bytes` companion tool to hex-dump pkt#104 in
//  10-byte windows, cross-referencing with the in-game capture state.
//
//  LAYER:   Protocol / tools
//  SESSION: April 2026 (post-RE, Tier-1 deliverable)
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

namespace aoc { namespace protocol { namespace tools {
// C | Y | B and C-O-A

struct PatchRule {
    std::string          name;        // human-readable identifier
    std::vector<uint8_t> needle;      // captured bytes to find
    std::vector<uint8_t> replacement; // bytes to write (MUST be same length)
    int                  expected_hits = -1;  // -1 = don't care; >=0 = sanity check
    int                  max_safe_hits = 10;  // abort rule if hits exceed this (prevents
                                              // corrupting common int32=1 values across
                                              // thousands of unrelated packets)
    bool                 enabled  = true;
};

struct PatchReport {
    std::string name;
    size_t      applied_count = 0;
    bool        sanity_ok     = true;   // false if expected_hits != applied_count
    std::string note;
};

class ReplayPropertyPatcher {
public:
    /// Add a raw-bytes patch rule (needle and replacement must be same length).
    void add_raw(std::string name,
                 std::vector<uint8_t> needle,
                 std::vector<uint8_t> replacement,
                 int expected_hits = -1) {
        if (needle.size() != replacement.size()) {
            // Silently skip — guard against caller bugs
            return;
        }
        rules_.push_back({std::move(name), std::move(needle),
                          std::move(replacement), expected_hits, true});
    }

    /// Add an int32 LE patch.
    void add_int32(std::string name, int32_t captured, int32_t new_val,
                   int expected_hits = -1) {
        std::vector<uint8_t> n(4), r(4);
        std::memcpy(n.data(), &captured, 4);
        std::memcpy(r.data(), &new_val,  4);
        add_raw(std::move(name), std::move(n), std::move(r), expected_hits);
    }

    /// Add a uint32 LE patch.
    void add_uint32(std::string name, uint32_t captured, uint32_t new_val,
                    int expected_hits = -1) {
        std::vector<uint8_t> n(4), r(4);
        std::memcpy(n.data(), &captured, 4);
        std::memcpy(r.data(), &new_val,  4);
        add_raw(std::move(name), std::move(n), std::move(r), expected_hits);
    }

    /// Add a float LE patch.
    void add_float(std::string name, float captured, float new_val,
                   int expected_hits = -1) {
        std::vector<uint8_t> n(4), r(4);
        std::memcpy(n.data(), &captured, 4);
        std::memcpy(r.data(), &new_val,  4);
        add_raw(std::move(name), std::move(n), std::move(r), expected_hits);
    }

    /// Add an int8 patch.
    void add_int8(std::string name, int8_t captured, int8_t new_val,
                  int expected_hits = -1) {
        add_raw(std::move(name), {uint8_t(captured)}, {uint8_t(new_val)},
                expected_hits);
    }

    /// Add a uint8 patch.
    void add_uint8(std::string name, uint8_t captured, uint8_t new_val,
                   int expected_hits = -1) {
        add_raw(std::move(name), {captured}, {new_val}, expected_hits);
    }

    /// Add a patch with a longer anchor context (reduces false matches).
    /// anchor_prefix bytes are matched but NOT replaced; captured/new_val
    /// bytes are at position anchor_prefix.size() inside the needle.
    void add_int32_anchored(std::string name,
                             const std::vector<uint8_t>& anchor_prefix,
                             int32_t captured,
                             int32_t new_val,
                             const std::vector<uint8_t>& anchor_suffix = {},
                             int expected_hits = -1) {
        std::vector<uint8_t> n, r;
        n.reserve(anchor_prefix.size() + 4 + anchor_suffix.size());
        r.reserve(n.capacity());
        n.insert(n.end(), anchor_prefix.begin(), anchor_prefix.end());
        r.insert(r.end(), anchor_prefix.begin(), anchor_prefix.end());
        uint8_t cbuf[4], nbuf[4];
        std::memcpy(cbuf, &captured, 4);
        std::memcpy(nbuf, &new_val,  4);
        n.insert(n.end(), cbuf, cbuf + 4);
        r.insert(r.end(), nbuf, nbuf + 4);
        n.insert(n.end(), anchor_suffix.begin(), anchor_suffix.end());
        r.insert(r.end(), anchor_suffix.begin(), anchor_suffix.end());
        add_raw(std::move(name), std::move(n), std::move(r), expected_hits);
    }

    /// Count how many rules are enabled.
    size_t enabled_rule_count() const {
        size_t n = 0;
        for (auto& r : rules_) if (r.enabled) ++n;
        return n;
    }

    /// Disable a rule by name.
    void disable(const std::string& name) {
        for (auto& r : rules_) if (r.name == name) r.enabled = false;
    }

    /// Apply all enabled rules across a list of packets.
    /// Packet type must have a `.raw` vector<uint8_t> or similar — we accept
    /// any type with a [raw member of type with data()/size()].  Users of
    /// this class typically pass ReplayData::packets.
    ///
    /// SAFETY: uses a two-pass approach:
    ///   Pass 1 — COUNT matches for each rule without modifying packets.
    ///            If count > max_safe_hits, abort that rule (it's hitting
    ///            common generic bytes like int32=1 everywhere and would
    ///            corrupt unrelated packets).
    ///   Pass 2 — APPLY safe rules (count <= max_safe_hits).
    template <typename PacketVec>
    std::vector<PatchReport> apply_all(PacketVec& packets) {
        std::vector<PatchReport> reports;
        reports.reserve(rules_.size());

        for (const auto& rule : rules_) {
            PatchReport report;
            report.name = rule.name;
            if (!rule.enabled) {
                report.note = "disabled";
                reports.push_back(std::move(report));
                continue;
            }
            if (rule.needle.empty() ||
                rule.needle.size() != rule.replacement.size()) {
                report.note = "invalid rule";
                reports.push_back(std::move(report));
                continue;
            }
// C-O-A - Cyb

            // ── PASS 1: COUNT without modifying ──
            size_t match_count = 0;
            for (auto& pkt : packets) {
                const auto& raw = pkt.raw;
                if (raw.size() < rule.needle.size()) continue;
                const size_t nsz = rule.needle.size();
                for (size_t i = 0; i + nsz <= raw.size(); ++i) {
                    if (std::memcmp(raw.data() + i, rule.needle.data(), nsz)
                        == 0) {
                        ++match_count;
                        // Quick-fail: if we already exceed the safety cap
                        // by a large margin, stop counting.
                        if (match_count > static_cast<size_t>(rule.max_safe_hits) * 3) {
                            goto count_done;
                        }
                        i += nsz - 1;
                    }
                }
            }
            count_done:

            if (match_count == 0) {
                report.sanity_ok = true;
                report.note = "0 matches — captured_* value likely wrong";
                reports.push_back(std::move(report));
                continue;
            }

            if (match_count > static_cast<size_t>(rule.max_safe_hits)) {
                // SAFETY ABORT: too many matches, skip this rule.
                // (Patching would corrupt unrelated int32=N values all over.)
                report.sanity_ok = false;
                report.note = "SAFETY ABORT — " + std::to_string(match_count) +
                              " matches exceeds max_safe_hits=" +
                              std::to_string(rule.max_safe_hits) +
                              " (captured value too generic; patch skipped)";
                reports.push_back(std::move(report));
                continue;
            }

            // ── PASS 2: APPLY (safe — count within limit) ──
            for (auto& pkt : packets) {
                auto& raw = pkt.raw;
                if (raw.size() < rule.needle.size()) continue;
                const size_t nsz = rule.needle.size();
                for (size_t i = 0; i + nsz <= raw.size(); ++i) {
                    if (std::memcmp(raw.data() + i, rule.needle.data(), nsz)
                        == 0) {
                        std::memcpy(raw.data() + i, rule.replacement.data(),
                                    nsz);
                        ++report.applied_count;
                        i += nsz - 1;
                    }
                }
            }

            if (rule.expected_hits >= 0 &&
                static_cast<int>(report.applied_count) != rule.expected_hits) {
                report.sanity_ok = false;
                report.note = "hit count mismatch (expected " +
                              std::to_string(rule.expected_hits) +
                              ", got " + std::to_string(report.applied_count) +
                              ")";
            }
            reports.push_back(std::move(report));
        }
        return reports;
    }

    /// Format all reports as a single multi-line summary string.
    static std::string format_report(const std::vector<PatchReport>& rps) {
        std::string out;
        for (const auto& r : rps) {
            out += "  ";
            out += r.name;
            out += ": ";
            out += std::to_string(r.applied_count);
            out += " patch(es)";
            if (!r.sanity_ok) {
                out += " [SANITY WARN: ";
                out += r.note;
                out += "]";
            } else if (!r.note.empty()) {
                out += " [";
                out += r.note;
                out += "]";
            }
            out += "\n";
        }
        return out;
    }

    /// Direct access (for debugging).
    const std::vector<PatchRule>& rules() const { return rules_; }

private:
    std::vector<PatchRule> rules_;
};

}}} // namespace aoc::protocol::tools

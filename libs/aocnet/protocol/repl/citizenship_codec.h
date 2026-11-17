// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/citizenship_codec.h
//
//  FEssentialCitizenshipWindowData (ClientPopulateCitizenshipData), the citizenship window's data: its own NetSerialize, read from the client (the struct's
//  TCppStructOps NetSerialize, slot 10 of its vtable; the struct is 0x168 bytes and has no reflected member).  An archive: raw words, LSB first; a
//  count or an int32 32 bits, an int64 or a double 64, a byte 8, a bool a 32-bit word that must be 0 or 1 (the exe marks a larger one as an error), an FString as
//  codecs.h's read_fstring, an FGuid four 32-bit words.  The members' names are the usmap's where the struct is a reflected one (FCitizenshipStatus,
//  FNodeNotificationDetails, FCitizenshipDuesData, FGuidNamePair): every type and offset of the usmap's order matches the code's.
//
//    32 + n x notification    +0xb0 TArray<FNodeNotificationDetails> (0x80 each; the exe refuses a count above 0x20000):
//        8                        NodeNotificationType
//        64 + 64                  CreationTimestamp, Timestamp
//        FString                  MayorName
//        8 + 8                    ElectionState (+0x29), then ElectionType (+0x28): the code runs them in that order
//        64 + 64                  ServiceBuildingPlotRecordId, ServiceBuildingRecordId
//        8                        ServiceBuildingState (+0x48)
//        64                       ServiceBuildingExpansionRecordId (+0x40)
//        32                       ServiceBuildingExpansionState
//        64 x 4                   MayoralCommissionRecordId, DeclaringNodeGuid, TargetNodeGuid, NodeWarRecordId
//        32                       bDidWinWar (a bool word)
//        8                        WarEndReason
//        64                       BuyOrderRecordId
//    8                        +0x00 a byte: the window compares it with a notification's ElectionState (1 to 3) and shows that election
//    32 + n x ...             +0x08 a TMap<FGuid, {FString, TArray<FGuid>}>: each key (FGuid), its FString, then an int32 count and that many FGuids
//    32 + n x ...             +0x58 a TMap<FGuid, FString>: each key (FGuid), then its FString
//    32                       +0xa8 an int32
//    ...                      +0xc0 FCitizenshipStatus:
//        FGuid + FString          PrimaryCharacter (FGuidNamePair)
//        32 + n x (FGuid+FString) AlternateCharacters (the exe refuses a count above 0x80000)
//        32 + 32                  DuesPaid, DuesRequired
//        64 x 3                   DuesLevelImpact, DuesCitizenshipRankImpact, DuesNonPropertyOwnerImpact (doubles)
//        64                       CitizenshipAtRiskStartTime (+0x80; a double)
//        32                       bDuesOverdue (a bool word)
//        32 x 3                   CitizenshipRank, PlayerLevel, HighestCharacterLevelOnAccount
//        8                        PropertyOwnershipState
//        (PropertiesUniqueHash, CitizenshipAtRisk and LastTimePropertyTaxPaidInFull are not sent)
//    64 + 64 + 8              +0x150 FCitizenshipDuesData: CitizenshipDuesDueTime, LateFeesDueTime, CitizenshipDuesState
//
//  No recording of the call is in this repository: the layout is the client's code alone.
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/quest_codec.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <bit>
#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// What worldd writes and reads at most (the exe's own limits are far larger: 0x20000 notifications, 0x80000 alternates).
constexpr size_t kMaxCitizenshipNotifications = 256;
constexpr size_t kMaxCitizenshipEntries = 4096;   // each map's entries
constexpr size_t kMaxCitizenshipGuids = 4096;     // one entry's guid list
constexpr size_t kMaxCitizenshipAlternates = 64;

inline bool citizenship_window_fits(const CitizenshipWindowData& d) {
    if (d.notifications.size() > kMaxCitizenshipNotifications || d.named_guids.size() > kMaxCitizenshipEntries || d.names.size() > kMaxCitizenshipEntries ||
        d.status.alternates.size() > kMaxCitizenshipAlternates)
        return false;
    for (const NodeNotification& n : d.notifications) {
        if (!fstring_fits(n.mayor)) return false;
    }
    for (const CitizenshipNamedGuids& e : d.named_guids) {
        if (!fstring_fits(e.name) || e.guids.size() > kMaxCitizenshipGuids) return false;
    }
    for (const auto& e : d.names) {
        if (!fstring_fits(e.second)) return false;
    }
    if (!fstring_fits(d.status.primary.name)) return false;
    for (const GuidNamePair& a : d.status.alternates) {
        if (!fstring_fits(a.name)) return false;
    }
    return true;
}

/// An FGuid as its four 32-bit words (A, B, C, D), as the archive writes it.
inline void write_guid_words(emit::BunchWriter& out, const GuidValue& g) {
    out.write_uint32(g.a);
    out.write_uint32(g.b);
    out.write_uint32(g.c);
    out.write_uint32(g.d);
}

inline GuidValue read_guid_words(wire::PacketReader& r) {
    GuidValue g;
    g.a = r.read_uint32();
    g.b = r.read_uint32();
    g.c = r.read_uint32();
    g.d = r.read_uint32();
    return g;
}

inline void write_citizenship_double(emit::BunchWriter& out, double v) { out.write_uint64(std::bit_cast<uint64_t>(v)); }
inline double read_citizenship_double(wire::PacketReader& r) { return std::bit_cast<double>(r.read_uint64()); }

/// A bool as the archive's 32-bit word; nullopt for a word above 1 (the exe's error).
inline std::optional<bool> read_citizenship_bool(wire::PacketReader& r) {
    const uint32_t w = r.read_uint32();
    if (w > 1) return std::nullopt;
    return w != 0;
}

inline bool write_citizenship_window(emit::BunchWriter& out, const CitizenshipWindowData& d) {
    if (!citizenship_window_fits(d)) return false;
    out.write_uint32(static_cast<uint32_t>(d.notifications.size()));
    for (const NodeNotification& n : d.notifications) {
        out.write_uint8(n.type);
        out.write_uint64(static_cast<uint64_t>(n.created));
        out.write_uint64(static_cast<uint64_t>(n.timestamp));
        if (!write_fstring(out, n.mayor)) return false;
        out.write_uint8(n.election_state);
        out.write_uint8(n.election_type);
        out.write_uint64(static_cast<uint64_t>(n.plot));
        out.write_uint64(static_cast<uint64_t>(n.building));
        out.write_uint8(n.building_state);
        out.write_uint64(static_cast<uint64_t>(n.expansion));
        out.write_int32(n.expansion_state);
        out.write_uint64(static_cast<uint64_t>(n.commission));
        out.write_uint64(static_cast<uint64_t>(n.declaring_node));
        out.write_uint64(static_cast<uint64_t>(n.target_node));
        out.write_uint64(static_cast<uint64_t>(n.war));
        out.write_uint32(n.won_war ? 1u : 0u);
        out.write_uint8(n.war_end_reason);
        out.write_uint64(static_cast<uint64_t>(n.buy_order));
    }
    out.write_uint8(d.state);
    out.write_uint32(static_cast<uint32_t>(d.named_guids.size()));
    for (const CitizenshipNamedGuids& e : d.named_guids) {
        write_guid_words(out, e.key);
        if (!write_fstring(out, e.name)) return false;
        out.write_uint32(static_cast<uint32_t>(e.guids.size()));
        for (const GuidValue& g : e.guids) write_guid_words(out, g);
    }
    out.write_uint32(static_cast<uint32_t>(d.names.size()));
    for (const auto& e : d.names) {
        write_guid_words(out, e.first);
        if (!write_fstring(out, e.second)) return false;
    }
    out.write_int32(d.phase_seconds_left);
    const CitizenshipStatus& s = d.status;
    write_guid_words(out, s.primary.guid);
    if (!write_fstring(out, s.primary.name)) return false;
    out.write_uint32(static_cast<uint32_t>(s.alternates.size()));
    for (const GuidNamePair& a : s.alternates) {
        write_guid_words(out, a.guid);
        if (!write_fstring(out, a.name)) return false;
    }
    out.write_int32(s.dues_paid);
    out.write_int32(s.dues_required);
    write_citizenship_double(out, s.dues_level_impact);
    write_citizenship_double(out, s.dues_rank_impact);
    write_citizenship_double(out, s.dues_non_owner_impact);
    write_citizenship_double(out, s.at_risk_start);
    out.write_uint32(s.dues_overdue ? 1u : 0u);
    out.write_int32(s.rank);
    out.write_int32(s.player_level);
    out.write_int32(s.highest_level);
    out.write_uint8(s.property_state);
    out.write_uint64(static_cast<uint64_t>(d.dues_due_time));
    out.write_uint64(static_cast<uint64_t>(d.late_fees_due_time));
    out.write_uint8(d.dues_state);
    return true;
}

inline std::optional<CitizenshipWindowData> read_citizenship_window(wire::PacketReader& r) {
    CitizenshipWindowData d;
    const auto count = [&r](size_t most) -> std::optional<size_t> {
        const int32_t n = static_cast<int32_t>(r.read_uint32());
        if (r.overflowed() || n < 0 || static_cast<size_t>(n) > most) return std::nullopt;
        return static_cast<size_t>(n);
    };
    const auto notes = count(kMaxCitizenshipNotifications);
    if (!notes) return std::nullopt;
    for (size_t i = 0; i < *notes; ++i) {
        NodeNotification n;
        n.type = r.read_uint8();
        n.created = static_cast<int64_t>(r.read_uint64());
        n.timestamp = static_cast<int64_t>(r.read_uint64());
        auto mayor = read_fstring(r);
        if (!mayor) return std::nullopt;
        n.mayor = std::move(*mayor);
        n.election_state = r.read_uint8();
        n.election_type = r.read_uint8();
        n.plot = static_cast<int64_t>(r.read_uint64());
        n.building = static_cast<int64_t>(r.read_uint64());
        n.building_state = r.read_uint8();
        n.expansion = static_cast<int64_t>(r.read_uint64());
        n.expansion_state = static_cast<int32_t>(r.read_uint32());
        n.commission = static_cast<int64_t>(r.read_uint64());
        n.declaring_node = static_cast<int64_t>(r.read_uint64());
        n.target_node = static_cast<int64_t>(r.read_uint64());
        n.war = static_cast<int64_t>(r.read_uint64());
        const auto won = read_citizenship_bool(r);
        if (!won) return std::nullopt;
        n.won_war = *won;
        n.war_end_reason = r.read_uint8();
        n.buy_order = static_cast<int64_t>(r.read_uint64());
        d.notifications.push_back(std::move(n));
    }
    d.state = r.read_uint8();
    const auto named = count(kMaxCitizenshipEntries);
    if (!named) return std::nullopt;
    for (size_t i = 0; i < *named; ++i) {
        CitizenshipNamedGuids e;
        e.key = read_guid_words(r);
        auto nm = read_fstring(r);
        if (!nm) return std::nullopt;
        e.name = std::move(*nm);
        const auto guids = count(kMaxCitizenshipGuids);
        if (!guids) return std::nullopt;
        for (size_t j = 0; j < *guids; ++j) e.guids.push_back(read_guid_words(r));
        d.named_guids.push_back(std::move(e));
    }
    const auto names = count(kMaxCitizenshipEntries);
    if (!names) return std::nullopt;
    for (size_t i = 0; i < *names; ++i) {
        const GuidValue key = read_guid_words(r);
        auto nm = read_fstring(r);
        if (!nm) return std::nullopt;
        d.names.emplace_back(key, std::move(*nm));
    }
    d.phase_seconds_left = static_cast<int32_t>(r.read_uint32());
    CitizenshipStatus& s = d.status;
    s.primary.guid = read_guid_words(r);
    auto primary = read_fstring(r);
    if (!primary) return std::nullopt;
    s.primary.name = std::move(*primary);
    const auto alts = count(kMaxCitizenshipAlternates);
    if (!alts) return std::nullopt;
    for (size_t i = 0; i < *alts; ++i) {
        GuidNamePair a;
        a.guid = read_guid_words(r);
        auto nm = read_fstring(r);
        if (!nm) return std::nullopt;
        a.name = std::move(*nm);
        s.alternates.push_back(std::move(a));
    }
    s.dues_paid = static_cast<int32_t>(r.read_uint32());
    s.dues_required = static_cast<int32_t>(r.read_uint32());
    s.dues_level_impact = read_citizenship_double(r);
    s.dues_rank_impact = read_citizenship_double(r);
    s.dues_non_owner_impact = read_citizenship_double(r);
    s.at_risk_start = read_citizenship_double(r);
    const auto overdue = read_citizenship_bool(r);
    if (!overdue) return std::nullopt;
    s.dues_overdue = *overdue;
    s.rank = static_cast<int32_t>(r.read_uint32());
    s.player_level = static_cast<int32_t>(r.read_uint32());
    s.highest_level = static_cast<int32_t>(r.read_uint32());
    s.property_state = r.read_uint8();
    d.dues_due_time = static_cast<int64_t>(r.read_uint64());
    d.late_fees_due_time = static_cast<int64_t>(r.read_uint64());
    d.dues_state = r.read_uint8();
    if (r.overflowed()) return std::nullopt;
    return d;
}

}}}  // namespace aoc::protocol::repl

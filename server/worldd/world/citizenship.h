// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/citizenship.h  (node citizenship, stage 1)
//
//  The rules of node citizenship, from content.db (contentc's citizenship_generic, citizenship_node and citizenship_dues_def, from the design data's
//  CitizenshipGenericRecordBase, CitizenshipNodeRecordBase and CitizenshipDuesDefRecordBase):
//
//    * a node takes citizens at the levels citizenship_node lists (Village 3 and Town 4 in the shipped data), each with its CitizenshipCooldownTimeSeconds (0 in the data);
//    * joining costs one Tax Receipt: the join dialog's CitizenshipCost (1, and one Tax Receipt is taken), which the client checks against the
//      TaxReceipts the player holds (the TaxVoucher items, sold by the town hall's administrator for 4 Glint each).  The data's InitialDueCost (20) and
//      InitialFeeCost (100) are not charged;
//    * dues: every FrequencyDuesPaidWeeks (2) weeks on the node's schedule: all of a node's citizens share its dates, to the second, and a new
//      citizen's first date is the node's next one (it may be days away).  They are paid for the coming date, any time before it; once it passes unpaid they are
//      overdue, and LateFee (5) more after OverduePeriodDays (5).  The data's DueDayOfTheWeek (Thursday) is not used: the dates are the node's;
//    * the Node Administrator's "become a citizen" line asks GetCharacterLevel(GetPlayer()) >= 10.
//
//    * dues unpaid past their overdue date put the citizenship at risk (CitizenshipAtRisk); after the node's CitizenshipAtRiskTimeLimitSeconds (7 days for nearly every node) more
//      it lapses (NumberOfMissableDuesPayments is 1: the first missed payment).  Modelled: the lapse is judged by the server's wall clock while a character of the account plays.
//
//  The dues of a period (CitizenshipDuesDefRecordBase, the data's own formulas): BaseDuesRequired (the BaseItemValue "CitizenshipDues": 1 Tax Receipt) times
//    * CitizenshipRankImpact: the citizen's rank / Threshold (25) once that is above 1 (the rank is its place among the node's citizens by when it joined);
//    * LevelImpact: the account's highest character level / Threshold (5) once that is above 1 (HighestCharacterLevelOnAccount);
//    * NonPropertyOwnerImpact (1);
//    * the node level's DuesNodeLevelScalar (CitizenshipNodeRecordBase: 1 for Village and Town),
//  rounded down, at least 1 (4.2 x 4.92 = 20.66 is 20).  The formulas' thresholds are read from their text (the "var Threshold = ..." each
//  declares).  Dues paid once overdue cost the node level's MissedDuesPercentIncrease more (0.5 Village, 0.75 Town), rounded down, and the LateFee.
//
//  Left out: CitizenshipGeneric's DuesPopulationCountScalars (x1.3 from 150 citizens, x2.0 from 300): the window of a large node shows no such factor.
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::world {

/// The TaxReceipts currency tier (CurrencyTierDefRecordBase "TaxReceipts"): its one kind is the TaxVoucher item.
constexpr int64_t kTaxReceiptsTier = 6064631941190189059;

/// The limits of a NodeTax tag in a node's tax table (TaxDetailsRecordBase): what its mayor may set.
struct TaxLimit {
    float   default_percent = 0.5f, min_percent = 0, max_percent = 1;
    int32_t default_flat = 0, min_flat = 0, max_flat = 0;
};

struct CitizenshipRules {
    int64_t dues_tier = kTaxReceiptsTier;   // what joining and the dues are paid in
    int32_t join_cost = 1;                  // the join dialog's CitizenshipCost: Tax Receipts to join
    int32_t dues_cost = 1;                  // CitizenshipDues: Tax Receipts each period
    int32_t dues_weeks = 2;                 // FrequencyDuesPaidWeeks
    int32_t late_fee = 5;                   // LateFee: Tax Receipts more once the dues are overdue
    int32_t overdue_days = 5;               // OverduePeriodDays
    int     min_level = 10;                 // the Node Administrator's line
    int32_t base_dues = 1;                  // BaseDuesRequired: the BaseItemValue's buy price in the dues tier
    double  rank_threshold = 25;            // CitizenshipRankImpact's Threshold
    double  level_threshold = 5;            // LevelImpact's Threshold
    double  non_owner_impact = 1;           // NonPropertyOwnerImpact
    std::map<int, double> dues_level_scalar;          // DuesNodeLevelScalar by node level (a level not listed: 1)
    std::map<int, double> missed_dues_increase;       // MissedDuesPercentIncrease by node level (not listed: 0)
    std::map<int64_t, int64_t> node_tax_table;                       // city node -> its NodeGlobalTaxTable (CityNodeRecordBase's TaxTable)
    std::map<int64_t, std::map<std::string, TaxLimit>> tax_limits;   // tax table -> NodeTax tag -> its limits
    /// The limits of `tag` in `node`'s table, from the tag up the tag tree (the first listed); null when the table lists none of them.
    const TaxLimit* tax_limit(int64_t node, const std::string& tag) const;
    std::map<int, int64_t> cooldown_secs;   // the node levels that take citizens, and their renounce cooldown
    std::map<int, int>     buy_order_slots; // citizenship_node's MinBuyOrderSlots by node level: the buy orders a node runs at once (a level not listed: none)
    int buy_order_slots_at(int level) const {
        const auto it = buy_order_slots.find(level);
        return it == buy_order_slots.end() ? 0 : it->second;
    }
    std::map<int64_t, int64_t> at_risk_secs;   // city_node.citizenship_at_risk_secs by node (a node not listed, or 0: kDefaultAtRiskSecs)
    static constexpr int64_t kDefaultAtRiskSecs = 604800;

    /// Whether a node at `level` takes citizens.
    bool takes_citizens(int level) const { return cooldown_secs.count(level) != 0; }
    /// The renounce cooldown of a node at `level` (ms; 0 for none or a level that takes no citizens).
    int64_t cooldown_ms(int level) const;
    /// One dues period (ms).
    int64_t period_ms() const { return static_cast<int64_t>(std::max(1, dues_weeks)) * 7 * 86400000; }
    /// A node's next dues date after `now_ms`: its schedule's start `anchor_ms` plus the fewest whole periods (at least one) that pass `now_ms`.
    int64_t next_dues_ms(int64_t anchor_ms, int64_t now_ms) const;
    /// When dues due at `due_ms` become overdue (the late fee is added from then on).
    int64_t overdue_ms(int64_t due_ms) const { return due_ms + static_cast<int64_t>(overdue_days) * 86400000; }
    /// The dues' multipliers for a citizen of rank `rank` whose account's highest character is at `level`, in a node at `node_level`.
    struct DuesFactors {
        double rank = 1, level = 1, non_owner = 1, node_level = 1;
    };
    DuesFactors dues_factors(int rank, int level, int node_level) const;
    /// One period's dues: base_dues times every factor, rounded down (at least 1).
    int32_t dues_amount(const DuesFactors& f) const;
    /// What paying the dues costs at `now_ms` when they are due at `due_ms`: `amount`, and once overdue the node level's increase and the late fee.
    int32_t dues_owed(int64_t due_ms, int64_t now_ms, int32_t amount, int node_level) const;
    /// How long a citizenship of `node` stays at risk before it lapses (ms).
    int64_t at_risk_ms(int64_t node) const {
        const auto it = at_risk_secs.find(node);
        return (it == at_risk_secs.end() || it->second <= 0 ? kDefaultAtRiskSecs : it->second) * 1000;
    }
    /// When a citizenship of `node` whose dues were due at `due_ms` lapses if they stay unpaid (0: never, no dues date).
    int64_t lapse_ms(int64_t node, int64_t due_ms) const { return due_ms > 0 ? overdue_ms(due_ms) + at_risk_ms(node) : 0; }

    /// Reads content.db's citizenship tables (read-only).  nullopt with `error` when the file has none (older than contentc version 12) or they do not read.
    static std::optional<CitizenshipRules> load(const std::string& content_db_path, std::string& error);
};

}  // namespace aoc::worldd::world

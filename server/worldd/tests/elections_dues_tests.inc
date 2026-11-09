// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/elections_dues_tests.inc: the dues and the node's dates, the mayor's taxes and the real tax limits
//  (included inside test_worldd_elections.cpp's anonymous namespace: its CHECK, kNode, the calendar's times and who())
// ============================================================================

void test_dues() {
    std::printf("the dues of the data's formulas\n");
    CitizenshipRules r;
    r.missed_dues_increase = {{3, 0.5}, {4, 0.75}};
    r.late_fee = 5;
    auto f = r.dues_factors(1, 10, 3);
    CHECK(f.rank == 1 && f.level == 2 && r.dues_amount(f) == 2, "the 1st citizen at level 10: level / 5 = 2, so 2 Tax Receipts");
    f = r.dues_factors(1, 4, 3);
    CHECK(f.level == 1 && r.dues_amount(f) == 1, "below level 5 the level impact is 1");
    // rounded down
    f = r.dues_factors(123, 21, 3);
    CHECK(std::abs(f.level - 4.2) < 1e-9 && std::abs(f.rank - 4.92) < 1e-9 && r.dues_amount(f) == 20, "4.2 x 4.92 = 20.66 is 20 (rounded down)");
    f = r.dues_factors(2221, 14, 3);
    CHECK(std::abs(f.level - 2.8) < 1e-9 && std::abs(f.rank - 88.84) < 1e-9 && r.dues_amount(f) == 248, "2.8 x 88.84 = 248.75 is 248");
    f = r.dues_factors(10, 25, 4);
    CHECK(f.rank == 1 && r.dues_amount(f) == 5, "no factor for the node's citizen count");
    CHECK(r.dues_amount(CitizenshipRules::DuesFactors{0.5, 1, 1, 1}) == 1, "at least 1");
    const int64_t due = 1'800'000'000'000;
    CHECK(r.dues_owed(due, due - 1, 4, 3) == 4 && r.dues_owed(due, r.overdue_ms(due) - 1, 4, 3) == 4, "before the overdue date the dues are the period's");
    CHECK(r.dues_owed(due, r.overdue_ms(due), 4, 3) == 6 + 5 && r.dues_owed(due, r.overdue_ms(due), 3, 4) == 5 + 5,
          "once overdue: the node level's increase (Village half, Town three quarters: 3 x 1.75 = 5.25 is 5) and the late fee");
    // the node's schedule: one set of dates for its citizens, every period from its start, with its time of day
    const int64_t anchor = kNov10Noon + 17 * 60000 + 3000;   // 12:17:03
    CHECK(r.period_ms() == 14 * kDay && r.next_dues_ms(anchor, anchor) == anchor + 14 * kDay, "the first citizen's first date: a period on");
    CHECK(r.next_dues_ms(anchor, anchor + 12 * kDay) == anchor + 14 * kDay, "a citizen who joins 12 days on has the node's date, 2 days away, to the second");
    CHECK(r.next_dues_ms(anchor, anchor + 14 * kDay) == anchor + 28 * kDay && r.next_dues_ms(anchor, anchor + 14 * kDay - 1) == anchor + 14 * kDay,
          "once a date passes, the next period's date");
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : "dist/content/content.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] SKIP: no %s\n", path.c_str());
        return;
    }
    std::string why;
    const auto real = CitizenshipRules::load(path, why);
    CHECK(real && real->base_dues == 1 && real->rank_threshold == 25 && real->level_threshold == 5 && real->non_owner_impact == 1,
          "the real dues: 1 Tax Receipt, the rank's threshold 25 and the level's 5 read from the formulas, no non-owner impact");
    CHECK(real && real->missed_dues_increase.count(3) && std::abs(real->missed_dues_increase.at(3) - 0.5) < 1e-9 && real->dues_level_scalar.count(4) &&
              real->dues_level_scalar.at(4) == 1 && real->join_cost == 1 && real->dues_weeks == 2,
          "... the Village's 50% for missed dues, the node level scalars, one Tax Receipt to join, every 2 weeks");
}
AOC_REGISTER_TEST(test_dues);

void test_taxes() {
    std::printf("the mayor's taxes and the node's treasury\n");
    Elections e;
    CHECK(!e.taxes(kNode) && e.treasury(kNode) == 0, "a node starts with the data's rates and an empty treasury");
    e.set_tax(kNode, "NodeTax.ArtisanTax", TaxOverride{0.2f, 0});
    e.add_treasury(kNode, 150);
    e.add_treasury(kNode, -500);
    CHECK(e.taxes(kNode) && e.taxes(kNode)->at("NodeTax.ArtisanTax").percent == 0.2f && e.treasury(kNode) == 0, "a rate is kept; the treasury never goes below nothing");
    e.add_treasury(kNode, 75);
    std::string why;
    Elections back;
    CHECK(back.from_json(e.to_json(), why) && back.all() == e.all() && back.treasury(kNode) == 75, "the rates and the treasury are written to the file and read back");
    // a shop's rate: the mayor's for its tag or the nearest tag above it, else the data's default
    const auto* overrides = e.taxes(kNode);
    const aoc::worldd::vendor::TaxRate deep = aoc::worldd::vendor::node_tax_rate("NodeTax.ArtisanTax.AgriculturalSupplyBaseTax.SalesTax", overrides);
    const aoc::worldd::vendor::TaxRate other = aoc::worldd::vendor::node_tax_rate("NodeTax.GoodsTax", overrides);
    const aoc::worldd::vendor::TaxRate plain = aoc::worldd::vendor::node_tax_rate("NodeTax.ArtisanTax.AgriculturalSupplyBaseTax.SalesTax", nullptr);
    CHECK(deep.found && deep.percent == 0.2f, "a tag under the one the mayor set takes the mayor's rate");
    CHECK(plain.found && plain.percent == aoc::worldd::vendor::default_tax_rate("NodeTax.ArtisanTax.AgriculturalSupplyBaseTax.SalesTax").percent && other.percent == plain.percent,
          "the other tags keep the data's default");
    const auto table = aoc::worldd::vendor::node_table(kNode, overrides);
    size_t mine = 0;
    for (const auto& en : table.entries) mine += en.tag == "NodeTax.ArtisanTax" && en.tax_percent == 0.2f && en.flag_a && en.flag_b;
    CHECK(mine == 1 && table.entries.size() == aoc::worldd::vendor::default_table(kNode).entries.size(), "the node's table carries the mayor's rate, marked as changed");
}
AOC_REGISTER_TEST(test_taxes);

void test_tax_limits() {
    std::printf("the tax limits of the real content.db\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : "dist/content/content.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] SKIP: no %s\n", path.c_str());
        return;
    }
    constexpr int64_t kJoeva = 108646121231993;
    std::string why;
    const auto c = CitizenshipRules::load(path, why);
    const TaxLimit* l = c ? c->tax_limit(kJoeva, "NodeTax.ArtisanTax.AgriculturalSupplyBaseTax.SalesTax") : nullptr;
    CHECK(l && std::abs(l->min_percent - 0.1f) < 1e-6f && std::abs(l->max_percent - 1.0f) < 1e-6f && std::abs(l->default_percent - 0.5f) < 1e-6f,
          "Joeva's tax table: a mayor sets a rate from 10% to 100% (the default 50%)");
    CHECK(c && !c->tax_limit(kJoeva, "Something.Else"), "a tag outside the NodeTax tree has no limits");
    const auto r = ElectionRules::load(path, why);
    CHECK(r && r->citizens_for_election(kJoeva) == 10, "Joeva needs 10 citizens to elect a mayor (MayorMinimumCitizens)");
}
AOC_REGISTER_TEST(test_tax_limits);

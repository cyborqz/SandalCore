// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/elections_reputation_tests.inc: node reputation and the node rewards (world/node_reputation.h, world/node_rewards.h)
//  (included by elections_registered.inc)
// ============================================================================

void test_reputation_rules() {
    std::printf("node reputation: the ranks, the decay, the range and the rules of the real content.db\n");
    const ReputationRules r;
    CHECK(r.rank_of(-500) == 0 && r.rank_of(0) == 0 && r.rank_of(9999.9) == 0 && r.rank_of(10000) == 1 && r.rank_of(30000) == 2 && r.rank_of(60000) == 3 &&
              r.rank_of(100000) == 4 && r.rank_of(1e9) == 4,
          "the ranks: Peasant below 10,000 (negatives too), Commoner, Liege, Noble, Aristocrat at 100,000");
    CHECK(std::string(reputation_rank_name(0)) == "Peasant" && std::string(reputation_rank_name(4)) == "Aristocrat" && std::string(reputation_rank_name(kNoReputationRank)) == "cCount",
          "... named as ENodeReputationRank, cCount for none");
    CHECK(r.tax_of(0) == 1.0 && r.tax_of(4) == 0.8 && r.tax_of(kNoReputationRank) == 1.0, "a rank's tax modifier; none for no rank");
    NodeRep fresh{100, 0};
    CHECK(!r.decay(fresh, 10 * kHour) && fresh.reputation == 100 && fresh.last_decay_ms == 10 * kHour, "a reputation that never decayed starts its clock");
    NodeRep up{100, 10 * kHour};
    CHECK(r.decay(up, 12 * kHour + kHour / 2) && up.reputation == 98 && up.last_decay_ms == 12 * kHour, "two and a half hours: 2 off, the half hour kept for the next");
    NodeRep down{-50, 10 * kHour};
    CHECK(r.decay(down, 13 * kHour) && down.reputation == -47, "a negative reputation decays toward 0 too");
    NodeRep small{0.5, 10 * kHour};
    CHECK(r.decay(small, 20 * kHour) && small.reputation == 0, "... and stops there");
    NodeRep later{100, 20 * kHour};
    CHECK(!r.decay(later, 10 * kHour) && later.reputation == 100 && later.last_decay_ms == 10 * kHour, "a clock from the future starts again now");
    NodeRep g{99000, 10 * kHour};
    CHECK(r.add(kNode, g, 5000, 10 * kHour) == 1000 && g.reputation == 100000, "a gain stops at the node's most (100,000 for a node the data does not list)");
    CHECK(r.add(kNode, g, -200000, 10 * kHour) == -100700 && g.reputation == -700, "... a loss at its least (-700)");
    CHECK(r.add(kNode, g, std::nan(""), 10 * kHour) == 0 && g.reputation == -700, "a NaN adds nothing");

    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : "dist/content/content.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] SKIP: no %s\n", path.c_str());
        return;
    }
    std::string why;
    const auto real = ReputationRules::load(path, why);
    CHECK(real.has_value(), "the reputation rules of the real content.db load");
    if (!real) return;
    CHECK(real->threshold[1] == 10000 && real->threshold[4] == 100000 && real->tax_modifier[1] == 0.95 && real->tax_modifier[4] == 0.8 && real->decay_per_hour == 1,
          "node_reputation_tier and node_constant: Commoner at 10,000 and 95% of the tax, Aristocrat at 100,000 and 80%; 1 an hour of decay");
    constexpr int64_t kHalcyon = 108646610851452ll, kDunir = 6064633651953926159ll, kLawlessDesert = 6064632734173233153ll;
    CHECK(real->range_of(kHalcyon) == std::make_pair(-700.0, 100000.0) && real->range_of(kDunir) == std::make_pair(-700.0, 1100.0) &&
              real->range_of(kLawlessDesert) == std::make_pair(0.0, 0.0),
          "city_node's range: Halcyon -700 to 100,000, the Dunir starting area to 1,100, the lawless desert none");
    NodeRep lawless{};
    CHECK(real->add(kLawlessDesert, lawless, 50, 10 * kHour) == 0 && lawless.reputation == 0, "a node of no range gives no reputation");
}

/// The node rewards' rules of the real content.db (world/node_rewards.h): the modifications of a reward, the boons and the commodities its node effects give.
void test_node_rewards() {
    std::printf("the node rewards: the rules of the real content.db\n");

    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : "dist/content/content.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] SKIP: no %s\n", path.c_str());
        return;
    }
    std::string why;
    const auto real = NodeRewards::load(path, why);
    if (!real) {
        std::printf("  [skip] SKIP: the content.db has no node reward tables (%s)\n", why.c_str());
        return;
    }
    constexpr int64_t kKillMinotaursReward = 6064632151087710232ll, kKillMinotaursMod = 6064633855459328091ll;   // zLegacy_MCOM_RVR_ANY_Kill_Minotaurs' RewardId
    const std::vector<RewardMod>* mods = real->mods_of(kKillMinotaursReward);
    CHECK(real->rewards() >= 100 && mods && mods->size() == 6 && mods->front().mod == kKillMinotaursMod && mods->front().duration_ms == 86400000,
          "node_reward_mod: a reward's six modifications in order, each for a day");
    bool boon = false;
    for (const RewardMod& m : mods ? *mods : std::vector<RewardMod>{}) {
        for (const NodeBoon& b : real->boons_of(m.mod)) boon = boon || (b.on_character && b.effect != 0);
    }
    CHECK(boon, "... and its modifications put a boon on characters");
    constexpr int64_t kCommodityMod = 6064633855548522597ll;
    const std::vector<NodeCommodityGift>& gifts = real->commodities_of(kCommodityMod);
    CHECK(gifts.size() == 2 && gifts[0].quantity + gifts[1].quantity == 100, "node_effect_commodity: a modification that gives the node's stores 80 and 20 units of two commodities");
}
AOC_REGISTER_TEST(test_reputation_rules);
AOC_REGISTER_TEST(test_node_rewards);

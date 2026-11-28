// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_remaps_tests.inc: the items whose ability reads a stat their block carries under another name
//  (the buff raises the stat the block carries — the one the item's stat line shows — with the value the item carries; the stat the ability reads stays as it was).
//  Registered with AOC_REGISTER_TEST; included from items_session_registered.inc.
//  Ported from war-drayv's pull request #22: the blocks that start with "Item remaps: contributed by war-drayv in #22." are theirs (a name, a number or a message
//  changed where the stat line moved a check); test_consumable_remap_rows, at the end, is ours.
// ============================================================================

// Item remaps: contributed by war-drayv in #22.
namespace gc {
inline constexpr int64_t kMightEffect = 6064632316175778138ll;   // the client's Arcane Surge effect (the item and the ability are the consumables' own)
inline constexpr int64_t kArcaneSurgePotion = 6064632111124054069ll;   // the real Arcane potion: its block carries MagicalPowerRating of its own
inline constexpr int64_t kSurgingMightPotion = 6064632111115337779ll, kSurgingMightAbility = 6064632316168765774ll,
                         kSurgingMightEffect = 6064632316168765776ll;   // the physical-power potion: its own ability reads its own PhysicalPowerRating
inline constexpr int64_t kMagicalPowerRating = 109183607068297ll;
inline constexpr int64_t kMagicalPenetration = 109342908372605ll, kPhysicalPenetration = 109342856795769ll;
inline constexpr int64_t kMagicalSpeed = 109183607068299ll, kPhysicalSpeed = 109342866232955ll;
inline constexpr int64_t kCraftingQualityRating = 6064630316223889411ll, kCraftingCostRating = 6064630316223627265ll;
inline constexpr int64_t kHuntingRarityRating = 6064630299332247618ll, kHuntingQuantityRating = 6064630299331854394ll,
                         kHuntingSpeedRating = 6064630299332902986ll;
}  // namespace gc

// Item remaps: contributed by war-drayv in #22.
/// One use of `item` at `quality` (its effects come with the cast, when its drinking or eating ends): what `read_stat` gained, and the value the item's stat
/// block carries under `carried_stat` at that quality.  A fixed item gives the same two numbers.
static std::pair<float, float> remap_gain(const char* path, int64_t item, int64_t ability, int64_t read_stat, int64_t carried_stat, int quality = 6,
                                          uint32_t instance = 730) {
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem used = make_item(Bag, 4, item, instance, 1);
    used.quality = quality;
    items.push_back(used);
    rec.items = items;
    Harness h(rec, true, true, path);
    const sg::CharacterStats* cs = h.s->character_stats();
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    if (!cs) return {-1.0f, -1.0f};
    const float before = cs->find(read_stat) ? cs->find(read_stat)->value : 0.0f;
    double carried = 0;
    for (const stats::StatMod& m : cs->item_stats(item, quality))
        if (m.stat == carried_stat) carried = m.value;
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 4);
    if (!held) return {-1.0f, -1.0f};
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(instance)), true),
                    h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, ability, 76, 4, items::to_instance(*held), 3), true)});
    for (int i = 0; i < 110; ++i) h.step();   // 2.2 s: the use runs its course
    const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(76) : -1;
    if (server >= 0) {
        h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the bar completes: the client ends its use (a food's or drink's buff waits for it)
        for (int i = 0; i < 15; ++i) h.step();
    }
    const sg::StatValue* after = cs->find(read_stat);
    return {after ? after->value - before : -1.0f, static_cast<float>(carried)};
}

// Item remaps: contributed by war-drayv in #22.
/// The effect record the client is shown the item's buff as: the one effect the mirror carries after the use (-1: not exactly one).
static int64_t shown_effect_guid(const char* path, int64_t item, int64_t ability, uint32_t instance, int32_t client_id) {
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem used = make_item(Bag, 4, item, instance, 1);
    used.quality = 1;
    items.push_back(used);
    rec.items = items;
    Harness h(rec, true, true, path);
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 4);
    if (!held) return 0;
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(instance)), true),
                    h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, ability, client_id, 4, items::to_instance(*held), 3), true)});
    for (int i = 0; i < 110; ++i) h.step();
    const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(client_id) : -1;
    if (server >= 0) {
        h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the bar completes: the client ends its use
        for (int i = 0; i < 15; ++i) h.step();
    }
    if (h.mirror.effect_items.size() != 1) return -1;
    return h.mirror.effect_items.begin()->second.guid;
}

// Item remaps: contributed by war-drayv in #22.
void test_consumable_remaps() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumable remaps: skipped (no real content.db)\n");
        return;
    }
    std::printf("The items whose ability reads a stat their block carries under another name give that number where the game puts it\n");

    // the Might Potion: Arcane Surge reads MagicalPowerRating and the item carries the number as PhysicalPowerRating — the buff is Physical Power, drawn as the
    // potion's own effect (Arcane Surge's record: the buff bar keeps the borrowed ability's text)
    {
        const auto might_physical = remap_gain(path, gc::kMightPotion, gc::kMightAbility, gc::kPhysicalPowerRating, gc::kPhysicalPowerRating, 1, 731);
        const auto might_magical = remap_gain(path, gc::kMightPotion, gc::kMightAbility, gc::kMagicalPowerRating, gc::kPhysicalPowerRating, 1, 736);
        std::printf("    the Might Potion raised PhysicalPowerRating by %.1f and MagicalPowerRating by %.1f (its block's PhysicalPowerRating: %.1f)\n",
                    might_physical.first, might_magical.first, might_physical.second);
        CHECK(might_physical.first == 50.0f && might_physical.second == 50.0f, "the Might Potion raises PhysicalPowerRating by its Common 50: the number its block carries");
        CHECK(might_magical.first == 0.0f, "... and leaves MagicalPowerRating alone (the ability's stat is remapped, not misapplied)");
        CHECK(shown_effect_guid(path, gc::kMightPotion, gc::kMightAbility, 732, 80) == gc::kMightEffect,
              "... and the buff is drawn as the potion's own effect (Arcane Surge's record): the physical-power record is not swapped in");
    }
    // the Arcane Surge and Surging Might potions keep their own numbers (nothing is remapped for them)
    CHECK(shown_effect_guid(path, gc::kArcaneSurgePotion, gc::kMightAbility, 733, 81) == gc::kMightEffect, "the Arcane Surge Potion keeps its own effect");
    CHECK(shown_effect_guid(path, gc::kSurgingMightPotion, gc::kSurgingMightAbility, 734, 82) == gc::kSurgingMightEffect,
          "the Surging Might Potion shows its own effect (nothing is remapped for it)");

    // the Elixir of Crafting Quality: the ability reads Quality, the item carries the number as Cost
    {
        const auto quality = remap_gain(path, 6064632105766944786ll, 6064632319994429485ll, gc::kCraftingQualityRating, gc::kCraftingCostRating);
        CHECK(quality.first == quality.second && quality.first == 40.0f, "the Crafting Quality elixir raises Quality by its Uncommon 40 (the block calls it Cost)");
    }
    // the Elixir of Hunting: the ability reads Rarity (and Speed), the item carries the Rarity number as Quantity, and the buff raises Quantity (and Speed)
    {
        const auto quantity = remap_gain(path, 6064632105783263255ll, 6064632320008192070ll, gc::kHuntingQuantityRating, gc::kHuntingQuantityRating);
        const auto speed = remap_gain(path, 6064632105783263255ll, 6064632320008192070ll, gc::kHuntingSpeedRating, gc::kHuntingSpeedRating, 6, 735);
        CHECK(quantity.first == quantity.second && quantity.first == 458.0f, "the Hunting elixir raises Quantity by its Uncommon 458: the number its block carries");
        CHECK(speed.first == speed.second && speed.first == 458.0f, "... and Speed by its own 458");
    }
    // the food and drinks of the same shape: the number stays on the stat their block carries
    {
        const auto tar_tar = remap_gain(path, 6064631986960924712ll, 6064632320263520601ll, gc::kPhysicalSpeed, gc::kPhysicalSpeed);
        CHECK(tar_tar.first == tar_tar.second && tar_tar.first > 0, "Beef Tar Tar raises PhysicalSpeedRating by the number its block carries");
        const auto goblet = remap_gain(path, 6064632586725298078ll, 6064632587116347463ll, gc::kMagicalSpeed, gc::kMagicalSpeed);
        CHECK(goblet.first == goblet.second && goblet.first > 0, "the Glyphic Goblet raises MagicalSpeedRating by the number its block carries");
    }
}
AOC_REGISTER_TEST(test_consumable_remaps);

namespace gc {
inline constexpr int64_t kPhysicalAccuracyRating = 109183605036677ll;
inline constexpr int64_t kProcessingRarity = 6064629302956720627ll, kProcessingRarityRating = 6064629302956720628ll;   // both read "Processing Quality"; only the Rating is on the character sheet
inline constexpr int64_t kTanningQuantityRating = 6064630299632336904ll, kTanningRarityRating = 6064630299632926740ll, kCarpentryQualityRating = 6064630248357101597ll;
}  // namespace gc

/// The stats the ability's effects read (what the item's description names), straight from the ability tables.
static std::set<int64_t> described_stats(const abilities::Catalog& catalog, int64_t ability) {
    std::set<int64_t> out;
    const abilities::AbilityDef* a = catalog.ability(ability);
    if (!a) return out;
    const auto add = [&](int64_t effect) {
        if (const abilities::EffectDef* e = catalog.effect(effect))
            for (const content::ModRow& m : e->mods) out.insert(m.stat_guid);
    };
    for (const auto& listed : a->effects) add(listed.second);
    for (const content::AbilityHitRow& hit : a->hits)
        if (const abilities::HitDef* h = catalog.hit(hit.hit_guid))
            for (const content::HitEffectRow& applied : h->effects) add(applied.effect_guid);
    return out;
}

/// One row per consumable the item remaps cover.  `block` is the item's stat line: every stat its block holds, with its number at the Uncommon quality (6).  `raised` is what the buff must raise, exactly,
/// each stat once by its number: the stat line, except for the Elixir of Crafting Quality, which follows its name (Crafting Quality, with the number its block holds as Crafting Cost).  `unraised` is the stat
/// that must stay at 0.  `described` is the stat the ability reads that the block does not hold (the description names it).
struct RemapRow {
    const char* name;
    int64_t     item, ability, hit, effect;   // hit: the hit whose effect is the buff when the ability lists no effect of its own (a scroll's), else 0; effect: the buff's effect record
    std::vector<std::pair<int64_t, float>> block, raised;
    int64_t     described, unraised;
    const char* stat_line;
    int64_t     not_in_buff = 0;   // when set: a stat of the block that the buff must not carry (the scroll's block names a base stat the sheet does not hold; the buff carries its Rating twin)
};

struct RowPlay {
    bool                                    ran = false;
    std::vector<std::pair<int64_t, float>>  gain;    // every stat the row names (raised, then unraised): the sheet after the use less before (-1: the sheet does not hold the stat)
    std::vector<stats::EffectMod>           buff;    // the mods of the buff's effect on the character, as the engine holds them
    std::vector<stats::StatMod>             block;   // the item's stat block at Uncommon
};

/// One use of a row's item at Uncommon (quality 6), played as the client sends it (the press and, for a scroll, the hit request; a food's or drink's buff waits for the end of its bar).
static RowPlay play_row(const char* path, const RemapRow& row, uint32_t instance) {
    RowPlay out;
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem used = make_item(Bag, 4, row.item, instance, 1);
    used.quality = 6;
    items.push_back(used);
    rec.items = items;
    Harness h(rec, true, true, path);
    const sg::CharacterStats* cs = h.s->character_stats();
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 4);
    if (!cs || !held) return out;
    std::vector<int64_t> named;
    for (const auto& r_stat : row.raised) named.push_back(r_stat.first);
    named.push_back(row.unraised);
    std::vector<float> before;
    for (const int64_t stat : named) before.push_back(cs->find(stat) ? cs->find(stat)->value : 0.0f);
    out.block = cs->item_stats(row.item, 6);
    std::vector<fake::Bunch> sent;
    sent.push_back(h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(instance)), true));
    sent.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, row.ability, 76, 4, items::to_instance(*held), 3), true));
    if (row.hit != 0) sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, row.ability, gc::kSelfTargeting, row.hit), true));
    h.client_sends(std::move(sent));
    for (int i = 0; i < 110; ++i) h.step();   // 2.2 s: the use runs its course
    const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(76) : -1;
    if (server >= 0) {
        h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the bar completes: the client ends its use
        for (int i = 0; i < 15; ++i) h.step();
    }
    for (size_t i = 0; i < named.size(); ++i) {
        const sg::StatValue* after = cs->find(named[i]);
        out.gain.push_back({named[i], after ? after->value - before[i] : -1.0f});
    }
    for (const stats::ActiveEffect& e : cs->effects())
        if (e.effect.guid == row.effect) out.buff.insert(out.buff.end(), e.mods.begin(), e.mods.end());
    out.ran = true;
    return out;
}

void test_consumable_remap_rows() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumable remap rows: skipped (no real content.db)\n");
        return;
    }
    std::printf("Each item's buff raises exactly the stats its stat line shows, by their numbers at Uncommon (quality 6), and not the stat its description names\n");
    const RemapRow rows[] = {
        {"Might Potion", gc::kMightPotion, gc::kMightAbility, 0, gc::kMightEffect, {{gc::kPhysicalPowerRating, 55.0f}}, {{gc::kPhysicalPowerRating, 55.0f}}, gc::kMagicalPowerRating,
         gc::kMagicalPowerRating, "Phys Power 55"},
        // the one row that follows its name: the Elixir of Crafting Quality raises Crafting Quality by the number its block holds as Crafting Cost, and Crafting Cost stays 0
        {"Elixir of Crafting Quality", 6064632105766944786ll, 6064632319994429485ll, 0, 6064632319994429487ll, {{gc::kCraftingCostRating, 40.0f}}, {{gc::kCraftingQualityRating, 40.0f}},
         gc::kCraftingQualityRating, gc::kCraftingCostRating, "Crafting Cost 40, raised as Crafting Quality 40 (it follows its name)"},
        {"Elixir of Hunting", 6064632105783263255ll, 6064632320008192070ll, 0, 6064632320008192072ll, {{gc::kHuntingQuantityRating, 458.0f}, {gc::kHuntingSpeedRating, 458.0f}},
         {{gc::kHuntingQuantityRating, 458.0f}, {gc::kHuntingSpeedRating, 458.0f}}, gc::kHuntingRarityRating, gc::kHuntingRarityRating, "Hunting Spoils 458 and Hunting Speed 458"},
        {"Glyphic Goblet", 6064632586725298078ll, 6064632587116347463ll, 0, 6064632587116347465ll, {{gc::kMagicalSpeed, 1374.0f}, {gc::kTanningQuantityRating, 344.0f}},
         {{gc::kMagicalSpeed, 1374.0f}, {gc::kTanningQuantityRating, 344.0f}}, gc::kMagicalPenetration, gc::kMagicalPenetration, "Mag Speed 1374 and Tanning Spoils 344"},
        {"Woodchip Wheat", 6064632586725298063ll, 6064632587103371270ll, 0, 6064632587103371272ll, {{gc::kPhysicalSpeed, 1374.0f}, {gc::kCarpentryQualityRating, 344.0f}},
         {{gc::kPhysicalSpeed, 1374.0f}, {gc::kCarpentryQualityRating, 344.0f}}, gc::kPhysicalPenetration, gc::kPhysicalPenetration, "Phys Speed 1374 and Carpentry Quality 344"},
        {"Beef Tar Tar", 6064631986960924712ll, 6064632320263520601ll, 0, 6064632320263520603ll, {{gc::kPhysicalSpeed, 705.0f}}, {{gc::kPhysicalSpeed, 705.0f}}, gc::kPhysicalPenetration,
         gc::kPhysicalPenetration, "Phys Speed 705"},
        {"Grilled Raptor Steak", 6064631986911117338ll, 6064632320245432635ll, 0, 6064632320245432637ll, {{gc::kMagicalSpeed, 705.0f}}, {{gc::kMagicalSpeed, 705.0f}}, gc::kMagicalPenetration,
         gc::kMagicalPenetration, "Mag Speed 705"},
        {"Roasted Gryphon", 6064631986906071063ll, 6064632320277545360ll, 0, 6064632320277545362ll, {{gc::kMagicalSpeed, 1374.0f}}, {{gc::kMagicalSpeed, 1374.0f}}, gc::kMagicalPenetration,
         gc::kMagicalPenetration, "Mag Speed 1374"},
        {"Roasted Scorpion Skewers", 6064632983814602768ll, 6064632320246939973ll, 0, 6064632320246939975ll, {{gc::kPhysicalSpeed, 1374.0f}}, {{gc::kPhysicalSpeed, 1374.0f}},
         gc::kPhysicalAccuracyRating, gc::kPhysicalAccuracyRating, "Phys Speed 1374"},
        {"Tanner's Jerky Sticks", 6064631987022921790ll, 6064632320305463821ll, 0, 6064632320305463823ll, {{gc::kPhysicalSpeed, 1374.0f}, {gc::kTanningRarityRating, 344.0f}},
         {{gc::kPhysicalSpeed, 1374.0f}, {gc::kTanningRarityRating, 344.0f}}, gc::kPhysicalPenetration, gc::kPhysicalPenetration, "Phys Speed 1374 and Tanning Quality 344"},
        {"Wyrmling Tar Tar", 6064631986970165292ll, 6064632320287375780ll, 0, 6064632320287375782ll, {{gc::kPhysicalSpeed, 1374.0f}}, {{gc::kPhysicalSpeed, 1374.0f}}, gc::kPhysicalPenetration,
         gc::kPhysicalPenetration, "Phys Speed 1374"},
        {"Novice Scroll of Processing Quality", 6064634619058126947ll, 6064632320879362646ll, 6064632320879362647ll, 6064632320879362648ll, {{gc::kProcessingRarity, 8.0f}},
         {{gc::kProcessingRarityRating, 8.0f}}, gc::kProcessingXpRating, gc::kProcessingXpRating, "Processing Quality 8 (raised as its Rating twin, the one on the sheet)", gc::kProcessingRarity},
    };
    {
        // the premise: the ability's effect reads the stat the description names, and the block does not hold it
        svc::Character rec = cyb_record();
        Harness h(rec, true, true, path);
        const abilities::Catalog* catalog = h.fx.world->ability_catalog();
        CHECK(catalog != nullptr, "(the ability tables are loaded)");
        if (catalog) {
            for (const RemapRow& row : rows) {
                const std::set<int64_t> named = described_stats(*catalog, row.ability);
                bool block_has_described = false;
                for (const auto& stat : row.block) block_has_described = block_has_described || stat.first == row.described;
                CHECK(named.count(row.described) == 1 && !block_has_described,
                      (std::string(row.name) + ": its ability's effect reads the stat its description names, and its stat block does not hold that stat").c_str());
            }
        }
    }
    uint32_t instance = 760;
    for (const RemapRow& row : rows) {
        const RowPlay play = play_row(path, row, instance++);
        const std::string name = row.name;
        CHECK(play.ran, (name + ": the use is played").c_str());
        bool block_is_line = play.block.size() == row.block.size();
        for (const auto& stat : row.block) {
            bool found = false;
            for (const stats::StatMod& m : play.block) found = found || (m.stat == stat.first && m.value == stat.second);
            block_is_line = block_is_line && found;
        }
        CHECK(block_is_line, (name + ": its stat block holds exactly its stat line at Uncommon (" + row.stat_line + ")").c_str());
        // the buff's mods are what the row raises: each stat once, with its number, and no other stat
        bool buff_is_raised = play.buff.size() == row.raised.size();
        for (const auto& stat : row.raised) {
            int times = 0;
            for (const stats::EffectMod& m : play.buff) times += m.stat == stat.first && m.value == stat.second ? 1 : 0;
            buff_is_raised = buff_is_raised && times == 1;
        }
        CHECK(buff_is_raised, (name + ": the buff carries exactly what the row raises, each stat once and nothing else (" + row.stat_line + ")").c_str());
        for (size_t i = 0; i < row.raised.size() && i < play.gain.size(); ++i) {
            std::printf("    %s: stat %lld rose by %.1f\n", row.name, static_cast<long long>(play.gain[i].first), play.gain[i].second);
            CHECK(play.gain[i].second == row.raised[i].second,
                  (name + " raises its stat " + std::to_string(static_cast<long long>(row.raised[i].first)) + " by exactly " + std::to_string(static_cast<int>(row.raised[i].second))).c_str());
        }
        if (row.not_in_buff != 0) {
            bool carried = false;
            for (const stats::EffectMod& m : play.buff) carried = carried || m.stat == row.not_in_buff;
            CHECK(!carried, (name + ": the stat its block names (" + std::to_string(static_cast<long long>(row.not_in_buff)) + ", not on the sheet) is not among the buff's mods").c_str());
        }
        const float unraised_gain = play.gain.empty() ? -1.0f : play.gain.back().second;
        std::printf("    %s: the stat that must stay (%lld) rose by %.1f\n", row.name, static_cast<long long>(row.unraised), unraised_gain);
        CHECK(unraised_gain == 0.0f, (name + " leaves the stat " + std::to_string(static_cast<long long>(row.unraised)) + " unraised").c_str());
    }
}
AOC_REGISTER_TEST(test_consumable_remap_rows);

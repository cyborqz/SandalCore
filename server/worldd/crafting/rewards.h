// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  crafting/rewards.h  (crafting, steps 2 and 3: "collect it", "craft"; header only)
//
//  What a finished processing job makes and pays: its products, their quality, the profession's experience and Adventuring's.  Everything here is a pure function of the job (its recipe, its
//  length, what was paid, its guid) and the crafter's level, so a job shows and pays the same whenever it is looked at: when it ends, when its window is opened, when it is collected.
//  A quick make (step 3) is worked out the same way once, when it ends, from the stacks it took.
//
//  The products.  The recipe's reward table gives them for one unit; a job makes that many times its quantity.  Most tables give fixed items; the few that pick (a mount's training picks
//  one of two or more animals) pick once for the job, by the job's guid.  Items of one record are added up.
//
//  The quality.  Every product starts with the quality of what was paid: for each record the job took, the mean of its units' quality (a unit of a rarity band counts as the band's first
//  quality, since a job is saved with its bands and not with each stack's exact number), then the mean over the records, worked out in single precision as the client does and rounded UP to a
//  whole number (CeilToInt: the game's own rule, which the community crafting calculator's newest version shows for processing: "the game rounds the slot average of the input tiers up").  The
//  band that whole number is in is the rarity of the job's products, and what the chances below read (paid_mean).
//
//  The artisan ratings (artisan.h).  A job's units are made one by one: each unit of a product has a chance of one more (the quantity chance of its rarity) and, after that, a chance of
//  coming out one rarity higher, and of one more after that, until a step moves none or the product's top rarity is reached; a unit that stepped up has the first quality of its new
//  rarity, so one product can come in several qualities.  The rolls are drawn from the job's guid, so a job shows and pays the same whenever it is looked at.  The profession experience is
//  multiplied by the crafter's experience rating.
//
//  The experience.  The profession's: 50, 60 or 70 (Novice, Apprentice, Journeyman recipes; Master and Grand Master fall back to 50; Animal Husbandry ten times that) times 1, 1.5, 2, 2.5,
//  3, 3.5 or 4 by the rarity, times the experience rating's multiplier, rounded for one unit, times the quantity.  Adventuring's: what a kill at the crafter's level gives, a fifth of it for
//  processing (half for crafting), times the recipe tier's modifier at that level, times 1, 1.4, 2, 4, 8 or 20 by the rarity, rounded for one unit, times the quantity.  The rarity a job's
//  experience is worked out at is the mean rarity of what was paid, rounded down (recorded claims fit it; the calculator does not model experience).
//
//  The quick make (step 3).  Its product starts at the first quality of the rarity band that the mean rarity of what was paid falls in, on the product's own quality curve: for each
//  recipe slot (one input of the recipe, whatever records and rarities paid it) the rarities of the units it took averaged by count, then the mean over the slots; the reagent does not count.
//  Every product, a piece of gear or a stackable one, also takes its share of the band: the band's width times the part of the mean rarity past the whole rarity.  The recipe's quality formula
//  then adds the crafter's quality rating (the curve value).  All of it is carried exactly (a base of 4.56 and a bonus of 3.98 is 8.54) and the product's whole quality is the nearest whole
//  number to that, at the very last step (artisan.h quality_from): nothing is cut off before.  The profession's
//  experience is 100, 200 or 300 (Novice, Apprentice, Journeyman
//  recipes; the higher tiers fall back to 100) times 1, 2, 3, 4, 5, 6 by the product's rarity (1 for the last band) times the slot factor of the product (a chest 2, a head 1.8, a one-handed
//  weapon 2.4 ...; 1 for anything else) times the experience rating's multiplier; Adventuring's is a crafting kill's half of a kill at the crafter's level, as a job's.  One craft pays once.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "crafting/job.h"
#include "crafting/rules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::crafting {

/// The rarity band (Common 0 ... Artifact 6) a quality is in on the common curve, held inside no bound: for a product the data does not hold (items/rarity.h).
inline int rarity_of_quality(int quality) { return items::default_rarity_facts().rarity_of(quality); }

/// What the units a job was paid with come to, as the game's client works it out (and the community crafting calculator's newest version shows): for each resource the units' band-first qualities
/// (Common 1, Uncommon 6, Rare 16, Heroic 31, Epic 51, Legendary 71, Artifact 101) added up and divided by the units, then the mean over the resources, every step in single precision like the
/// client's; `quality` is that mean rounded UP to a whole number (CeilToInt), `rarity` the band the quality is in.  What they drive: the products' quality and rarity, and the chance of one more
/// unit and the chance of one rarity higher, which both read the unit's rarity.  A job that paid no item: 1, Common, 1.
struct PaidMean {
    float points = 1.0f;
    int   quality = 1;
    int   rarity = 0;   // Common 0 ... Artifact 6
};

inline PaidMean paid_mean(const std::vector<svc::JobResource>& paid) {
    float total = 0.0f;
    int resources = 0;
    for (const svc::JobResource& r : paid) {
        float slot = 0.0f;
        int64_t units = 0;
        for (const svc::JobRarity& x : r.rarities) {
            if (x.count < 1) continue;
            slot = slot + static_cast<float>(x.count) * static_cast<float>(band_lo(x.rarity));
            units += x.count;
        }
        if (units < 1) continue;
        total = total + slot / static_cast<float>(units);
        ++resources;
    }
    PaidMean out;
    if (resources == 0) return out;
    out.points = total / static_cast<float>(resources);
    out.quality = std::clamp(static_cast<int>(std::ceil(out.points)), 1, kMaxQuality);
    out.rarity = rarity_of_quality(out.quality);
    return out;
}

/// The mean rarity (Common 0 ... Artifact 6) of what a job was paid with: for each resource the rarities of its units averaged by count, then the mean over the resources; 0 for a job that paid
/// no item.  The experience of a job is worked out at this, rounded down.
inline double paid_mean_rarity(const std::vector<svc::JobResource>& paid) {
    double sum = 0;
    int records = 0;
    for (const svc::JobResource& r : paid) {
        double rarity = 0;
        int64_t units = 0;
        for (const svc::JobRarity& x : r.rarities) {
            if (x.count < 1) continue;
            rarity += static_cast<double>(std::clamp(x.rarity, 0, items::kRarityCount - 1)) * static_cast<double>(x.count);
            units += x.count;
        }
        if (units < 1) continue;
        sum += rarity / static_cast<double>(units);
        ++records;
    }
    return records == 0 ? 0.0 : sum / records;
}

/// A mean rarity (a crafted piece's, over the recipe's slots) as the whole rarity it starts in: the band it falls in (a hair of slack for the float sums: 2.9999999 is 3).
inline int whole_rarity(double mean) { return std::clamp(static_cast<int>(std::floor(mean + 1e-9)), 0, items::kRarityCount - 1); }

inline int64_t round_half_up(double v) { return static_cast<int64_t>(std::floor(v + 0.5)); }

inline constexpr double kProfessionXpByTier[3] = {50.0, 60.0, 70.0};
inline constexpr double kProfessionXpByRarity[7] = {1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0};
inline constexpr double kAdventuringXpByRarity[7] = {1.0, 1.4, 2.0, 4.0, 8.0, 20.0, 20.0};

/// The profession's experience for `units` units of a recipe of certification tier `tier` made at rarity `rarity`; `multiplier` is what the crafter's experience rating makes of it (artisan.h).  The
/// experience of one unit is a whole number (rounded half up), and the job pays it for each unit.  0 for a rule this server does not work out.
inline int64_t profession_xp(content::XpRule rule, int tier, int rarity, int64_t units, double multiplier = 1.0) {
    if (rule != content::XpRule::Processing && rule != content::XpRule::AnimalHusbandry) return 0;
    const double base = (tier >= 0 && tier < 3 ? kProfessionXpByTier[tier] : kProfessionXpByTier[0]) * (rule == content::XpRule::AnimalHusbandry ? 10.0 : 1.0);
    return round_half_up(base * kProfessionXpByRarity[std::clamp(rarity, 0, 6)] * multiplier) * units;
}

/// Adventuring's experience for the same job, by the crafter's level (one unit's is a whole number, rounded half up; the job pays it for each unit).
inline int64_t adventuring_xp(const content::CraftXpCurves& curves, content::XpRule rule, int tier, int level, int rarity, int64_t units) {
    if ((rule != content::XpRule::Processing && rule != content::XpRule::Crafting) || !curves.ok()) return 0;
    const double share = rule == content::XpRule::Crafting ? 0.5 : 0.2;
    const double lv = static_cast<double>(std::max(level, 1));
    const double kill = eval_points(curves.kill, lv);
    const double modifier = eval_points(curves.tier[std::clamp(tier, 0, 2)], lv);
    return round_half_up(kill * share * modifier * kAdventuringXpByRarity[std::clamp(rarity, 0, 6)]) * units;
}

/// One group of units a craft was paid with: the recipe slot it paid (Take::slot), a stack's quality, how many units of it and the rarity the client shows for the stack.
struct PaidUnits {
    int64_t slot = 0;
    int     quality = 1;
    int64_t units = 0;
    int     rarity = 0;   // Common 0 ... Artifact 6
};

/// The mean rarity of what a quick make was paid with: for each slot of the recipe the rarities of its units averaged by count (a slot paid with several records or rarities is one slot), then
/// the mean over the slots, each slot weighing the same whatever it holds; 0 for a craft that paid no material.  [calc] the community crafting calculator does the same sum.
inline double craft_mean_rarity(const std::vector<PaidUnits>& paid) {
    std::vector<std::pair<int64_t, std::pair<double, int64_t>>> by_slot;   // slot -> (rarity x units added up, units)
    for (const PaidUnits& p : paid) {
        if (p.units < 1) continue;
        auto at = std::find_if(by_slot.begin(), by_slot.end(), [&](const auto& x) { return x.first == p.slot; });
        if (at == by_slot.end()) {
            by_slot.push_back({p.slot, {0.0, 0}});
            at = by_slot.end() - 1;
        }
        at->second.first += static_cast<double>(std::clamp(p.rarity, 0, items::kRarityCount - 1)) * static_cast<double>(p.units);
        at->second.second += p.units;
    }
    if (by_slot.empty()) return 0.0;
    double sum = 0;
    for (const auto& s : by_slot) sum += s.second.first / static_cast<double>(s.second.second);
    return sum / static_cast<double>(by_slot.size());
}

/// The quality a crafted piece starts with, before the recipe's quality formula adds the crafter's rating, as the EXACT number: the first quality of the rarity band the mean rarity falls in, on the
/// product's own quality curve, plus the piece's share of the band, the band's width times the part of the mean rarity past the whole rarity (a mean of 3.7 over Heroic 31 to 50 is 31 and 13.37, a
/// mean of 0.5 over Common 1 to 5 is 1 and 2).  Every product has the share, a piece of gear and a stackable one alike (as the community crafting calculator does).  Nothing is cut off here: the
/// whole quality is made from the exact base and the rating's bonus together, at the very last step (artisan.h quality_from).
inline double craft_base_quality(double mean_rarity, const items::ItemRarityFacts& product) {
    const int rarity = whole_rarity(mean_rarity);
    const items::ItemRarityFacts& facts = product.has_curve ? product : items::default_rarity_facts();
    const items::QualityBand& band = facts.bands[static_cast<size_t>(rarity)];
    double quality = std::ceil(band.x - 1e-9);
    if (band.y > band.x) quality += (band.y - band.x) * std::max(0.0, mean_rarity - rarity);
    return std::clamp(quality, 1.0, static_cast<double>(kMaxQuality));
}

/// A quality on the tier scale the community crafting calculator shows: the number of the rarity it is in (Common 1 ... Legendary 6, Artifact 7) and the way through that rarity's band (the quality
/// less the band's first over the band's last less its first, never quite 1): quality 9 in Uncommon 6 to 15 is 2.3333, "Uncommon +33.3 %".  The game's own quality runs 1 to 200 in these bands.
inline double tier_scale(const items::ItemRarityFacts& facts, int quality) {
    const int rarity = facts.rarity_of(quality);
    return static_cast<double>(rarity) + 1.0 + std::min(facts.position_of(quality), rarity == 5 ? 0.99 : 0.9999);   // (the calculator stops a Legendary at .99)
}

/// The factor of the crafting experience that depends on what is made (Artisanship_Crafting_SlotMod): the first of these the product's tags (lower case, each with its parents) hold.
inline double slot_mod(const std::set<std::string>& tags) {
    static const struct {
        const char* tag;
        double      mod;
    } kSlots[] = {{"slot.2hand", 3.0},   {"slot.1hand", 2.4},    {"slot.offhand", 1.6}, {"slot.head", 1.8},    {"slot.shoulder", 1.6},         {"slot.chest", 2.0},   {"slot.forearms", 1.3},
                  {"slot.hands", 1.5},   {"slot.waist", 1.3},    {"slot.legs", 2.0},    {"slot.feet", 1.5},    {"slot.necklace", 1.2},         {"slot.earring", 1.1}, {"slot.ring", 1.1},
                  {"slot.back", 1.2},    {"slot.shirt", 1.5},    {"slot.pants", 1.5},   {"slot.toolbelt", 1.5}, {"item.gear.artisan.tool", 2.0}, {"item.bag", 1.2}};
    for (const auto& k : kSlots) {
        if (tags.count(k.tag) != 0) return k.mod;
    }
    return 1.0;
}

inline constexpr double kCraftXpByTier[3] = {100.0, 200.0, 300.0};
inline constexpr double kCraftXpByRarity[7] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 1.0};

/// The profession's experience for one craft of a recipe of certification tier `tier` whose product is of rarity band `rarity` and slot factor `slot`, times what the crafter's experience rating
/// makes of it (`multiplier`, artisan.h); 0 for a rule this server does not work out.
inline int64_t craft_profession_xp(content::XpRule rule, int tier, int rarity, double slot, double multiplier = 1.0) {
    if (rule != content::XpRule::Crafting) return 0;
    const double base = tier >= 0 && tier < 3 ? kCraftXpByTier[tier] : kCraftXpByTier[0];
    return round_half_up(base * kCraftXpByRarity[std::clamp(rarity, 0, 6)] * slot * multiplier);
}

/// A number from a job's guid for the picks of a reward table: the same job picks the same every time.
inline uint64_t seed_of(const std::string& job_guid) {
    uint64_t h = 1469598103934665603ull;
    for (const char c : job_guid) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}
inline uint64_t next_random(uint64_t& state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/// The products of `units` units of a recipe whose reward is `reward`.
inline std::vector<Product> roll_products(const content::CraftReward& reward, int64_t units, uint64_t seed) {
    std::vector<Product> out;
    const auto add = [&](const content::CraftRewardItem& it) {
        const int64_t quantity = it.quantity * units;
        if (quantity < 1) return;
        const auto have = std::find_if(out.begin(), out.end(), [&](const Product& p) { return p.item == it.item; });
        if (have != out.end()) have->quantity += quantity;
        else out.push_back({it.item, quantity});
    };
    uint64_t state = seed;
    for (const content::CraftRewardContainer& c : reward.containers) {
        std::vector<size_t> chosen;
        if (c.pick == content::CraftRewardContainer::Pick::All || c.rewards.size() <= 1) {
            for (size_t i = 0; i < c.rewards.size(); ++i) chosen.push_back(i);
        } else {
            std::vector<size_t> left;
            for (size_t i = 0; i < c.rewards.size(); ++i) left.push_back(i);
            const size_t want = std::min(left.size(), static_cast<size_t>(std::max(1, c.number)));
            while (chosen.size() < want && !left.empty()) {
                double total = 0;
                for (const size_t i : left) total += c.pick == content::CraftRewardContainer::Pick::WeightedRandom && i < c.weights.size() ? std::max(0.0, c.weights[i]) : 1.0;
                const double at = total > 0 ? static_cast<double>(next_random(state) >> 11) / 9007199254740992.0 * total : 0.0;
                double run = 0;
                size_t pick = 0;
                for (size_t k = 0; k < left.size(); ++k) {
                    run += c.pick == content::CraftRewardContainer::Pick::WeightedRandom && left[k] < c.weights.size() ? std::max(0.0, c.weights[left[k]]) : 1.0;
                    pick = k;
                    if (at < run) break;
                }
                chosen.push_back(left[pick]);
                left.erase(left.begin() + static_cast<std::ptrdiff_t>(pick));
            }
        }
        for (const size_t i : chosen) {
            for (const content::CraftRewardItem& it : c.rewards[i]) add(it);
        }
    }
    return out;
}

/// How many of `n` independent tries succeed when each does with chance `p`, drawn from `state` (the generator of next_random); 0 for no tries or no chance, all of them for a sure one.
inline int64_t binomial(uint64_t& state, int64_t n, double p) {
    if (n <= 0 || !(p > 0.0)) return 0;
    if (p >= 1.0) return n;
    int64_t successes = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (static_cast<double>(next_random(state) >> 11) / 9007199254740992.0 < p) ++successes;
    }
    return successes;
}

/// What the job makes and pays for a crafter of `level` whose artisan ratings for the recipe's profession were `ratings` when the job started; nullopt when its recipe is none of the data's.
/// `rarity_facts` gives an item's rarity rule (none: the common curve): a unit that steps up a rarity takes the first quality of the new one.
inline std::optional<JobRewards> roll_job(const content::CraftRecipeData& data, const svc::ProcessingJob& job, int level, const Ratings& ratings = {},
                                          const std::function<items::ItemRarityFacts(int64_t)>& rarity_facts = {}) {
    const content::CraftRecipe* recipe = data.recipe(job.recipe_guid);
    if (!recipe) return std::nullopt;
    JobRewards out;
    out.profession = recipe->profession;
    out.level = std::max(level, 1);
    const PaidMean mean = paid_mean(job.resources);   // the mean of what was paid, rounded up as the client does: the products' quality and rarity
    out.quality = mean.quality;
    out.quality_exact = mean.points;
    out.rarity = whole_rarity(paid_mean_rarity(job.resources));   // (the rarity the experience is worked out at)
    out.ratings = ratings;
    const bool artisan = is_processing(data.artisan, recipe->profession);   // the recipe's formulas read the crafter's processing ratings
    if (const content::CraftReward* reward = data.reward_of(*recipe)) {
        const uint64_t seed = seed_of(job.job_guid);
        out.products = roll_products(*reward, job.quantity, seed);
        for (Product& p : out.products) p.quality = out.quality;
        if (artisan && ratings.any() && (recipe->has_quantity_formula || recipe->has_quality_formula)) {
            // the units, one product at a time: the extra units, then the steps up (the rolls have their own draws, apart from the reward table's picks)
            uint64_t state = seed ^ 0x2545f4914f6cdd1dull;
            std::vector<Product> lines;
            for (const Product& made : out.products) {
                const items::ItemRarityFacts facts = rarity_facts ? rarity_facts(made.item) : items::default_rarity_facts();
                Product line = made;
                int rarity = facts.rarity_of(line.quality);
                if (recipe->has_quantity_formula) {
                    const int64_t extra = binomial(state, line.quantity, quantity_chance(data.artisan, ratings, rarity));
                    line.quantity += extra;
                    out.extra_units += extra;
                }
                const size_t first = lines.size();
                lines.push_back(line);
                if (recipe->has_quality_formula && facts.has_curve) {
                    size_t at = first;
                    while (rarity < facts.rarity_max && rarity < items::kRarityCount - 1) {
                        const int64_t up = binomial(state, lines[at].quantity, rarity_step_chance(data.artisan, ratings, rarity));
                        if (up < 1) break;
                        lines[at].quantity -= up;
                        ++rarity;
                        Product next = lines[at];
                        next.quantity = up;
                        next.quality = std::clamp(static_cast<int>(std::ceil(facts.bands[static_cast<size_t>(rarity)].x - 1e-9)), 1, kMaxQuality);
                        lines.push_back(next);
                        at = lines.size() - 1;
                        out.stepped_units += up;
                    }
                    // a line with no unit left is no product: every unit of it stepped up again, and that includes the middle line of a unit that stepped up twice (Uncommon, Rare, Heroic leaves the Rare
                    // line at 0: a job with such a line could not be collected)
                    lines.erase(std::remove_if(lines.begin() + static_cast<std::ptrdiff_t>(first), lines.end(), [](const Product& p) { return p.quantity < 1; }), lines.end());
                }
            }
            out.products = std::move(lines);
        }
        out.xp_multiplier = artisan ? xp_multiplier(data.artisan, ratings, true) : 1.0;
        out.profession_xp = profession_xp(reward->profession_rule, recipe->tier, out.rarity, job.quantity, out.xp_multiplier);
        out.adventuring_xp = adventuring_xp(data.xp_curves, reward->adventuring_rule, recipe->tier, out.level, out.rarity, job.quantity);
    }
    return out;
}

/// What a quick make makes and pays, for a crafter of `level` whose artisan ratings for the recipe's profession are `ratings`: the products of one unit of the recipe and the experience of one
/// craft.  The quality starts from what was paid (`paid`: craft_base_quality, exact) and a crafting recipe's quality formula adds the crafter's quality rating (exact too); the product's whole
/// quality is the nearest whole number to the sum.  `facts_of` gives what is known of an item (its tags and its quality bands: the product's rarity and slot factor come from them); it may
/// answer nothing, then the general bands and a slot factor of 1 stand.  nullopt when the recipe has no reward table.
inline std::optional<JobRewards> roll_craft(const content::CraftRecipeData& data, const content::CraftRecipe& recipe, const std::vector<PaidUnits>& paid, const Ratings& ratings, int level,
                                            uint64_t seed, const std::function<std::optional<content::ItemSaleFacts>(int64_t)>& facts_of) {
    const content::CraftReward* reward = data.reward_of(recipe);
    if (!reward) return std::nullopt;
    JobRewards out;
    out.profession = recipe.profession;
    out.level = std::max(level, 1);
    out.ratings = ratings;
    out.products = roll_products(*reward, 1, seed);
    std::optional<content::ItemSaleFacts> product;
    if (!out.products.empty() && facts_of) product = facts_of(out.products.front().item);
    const items::ItemRarityFacts* rarity_rule = product ? static_cast<const items::ItemRarityFacts*>(&*product) : &items::default_rarity_facts();
    const bool crafting = data.artisan.by_profession.count(recipe.profession) != 0 && !is_processing(data.artisan, recipe.profession);   // the recipe's formulas read the crafter's crafting ratings
    double exact = craft_base_quality(craft_mean_rarity(paid), *rarity_rule);
    if (crafting && recipe.has_quality_formula) {
        out.quality_added = quality_bonus(data.artisan, ratings);
        exact += out.quality_added;
    }
    out.quality_exact = exact;
    out.quality = std::clamp(quality_from(exact), 1, kMaxQuality);
    for (Product& p : out.products) p.quality = out.quality;
    out.rarity = product ? product->rarity_of(out.quality) : rarity_of_quality(out.quality);
    out.xp_multiplier = crafting ? xp_multiplier(data.artisan, ratings, false) : 1.0;
    out.profession_xp = craft_profession_xp(reward->profession_rule, recipe.tier, out.rarity, product ? slot_mod(product->tags) : 1.0, out.xp_multiplier);
    out.adventuring_xp = adventuring_xp(data.xp_curves, reward->adventuring_rule, recipe.tier, out.level, out.rarity, 1);
    return out;
}

}  // namespace aoc::worldd::crafting

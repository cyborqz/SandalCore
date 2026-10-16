// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/item_use.cpp (see item_use.h)
// ============================================================================
#include "session/item_use.h"

#include "bootstrap/character_setup.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace aoc::worldd::use {

int class_enum_of(const content::ContentDb& db, int64_t class_guid) {
    if (class_guid == 0) return 0;
    for (int id = 1; id <= 8; ++id) {
        std::string error;
        const auto cls = db.class_for_name(bootstrap::client_class_name(id), error);
        if (cls && cls->guid == class_guid) return id;
    }
    return 0;
}

namespace {

std::string with_commas(int64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > (v < 0 ? 1 : 0); i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

}  // namespace
// [Cyb + C O A]

Plan plan_use(const content::ContentDb& db, int64_t item_record, const Context& ctx) {
    Plan plan;
    const auto refuse = [&](std::string why) {
        plan.ok = false;
        plan.refusal = std::move(why);
        plan.items.clear();
        plan.adventuring_xp = 0;
        plan.set_class.reset();
        plan.professions_after = professions::Professions();
        plan.profession_changes.clear();
        return plan;
    };
    std::string error;
    const auto use = db.item_use(item_record, error);
    if (!use) {
        if (!error.empty()) return refuse("content.db failed: " + error);
        return refuse("item record " + std::to_string(item_record) + " is neither worn in a slot nor used (no ItemActivationArchetype)");
    }
    if (use->activation != "ActivateActionResponses") return refuse("the item's activation is " + use->activation + ", which stage 1 does not run");
    if (use->actions.empty()) return refuse("the item has an activation archetype and no action");
    plan.consume = use->consume_on_use;

    size_t items_granted = 0;
    // Gathering stage 2: the professions the use works on (a copy: the plan changes nothing of the character's), the profession XP lines it ran (for the settlement)
    professions::Professions work = ctx.held;
    struct XpLine {
        const professions::Profession* profession = nullptr;
        int64_t                        expr = 0;
    };
    std::vector<XpLine> xp_lines;
    bool cert_reward_noted = false;
    for (const int64_t action_guid : use->actions) {
        const auto action = db.action_response(action_guid, error);
        if (!action) {
            if (!error.empty()) return refuse("content.db failed: " + error);
            return refuse("the item names the action response " + std::to_string(action_guid) + ", which content.db does not have");
        }
        if (action->response_type == "SetPrimaryArchetype") {
            const int cls = class_enum_of(db, action->class_guid);
            if (cls == 0) return refuse(action->name + " names class record " + std::to_string(action->class_guid) + ", which is no player class");
            if (cls == ctx.class_enum) return refuse("the character is a " + action->class_name + " already");
            if (plan.set_class && *plan.set_class != cls) return refuse("the item sets two classes");
            plan.set_class = cls;
            plan.summary += (plan.summary.empty() ? "" : ", ") + std::string("class -> ") + action->class_name;
            continue;
        }
        if (action->response_type == "SetCertificationLevel") {
            if (!ctx.professions || !work.valid()) {
                return refuse("the action response " + action->name + " changes a profession's certification, and worldd has no profession tables (content.db is older than contentc 13?)");
            }
            const professions::CertAction* cert = ctx.professions->action(action->guid);
            const professions::Profession* prof = cert ? ctx.professions->find(cert->profession) : nullptr;
            if (!cert || !prof) {
                return refuse("the action response " + action->name + " is no certification action of a profession (content.db's action_certification)");
            }
            if (!action->reward_tables.empty() && !cert_reward_noted) {
                cert_reward_noted = true;
                plan.notes.push_back("the reward table of a certification action is not granted (the data names the same table, Alchemy's XP, in every one of them)");
            }
            if (cert->promote) {
                bool skipped = false;
                const professions::State before = work.state(prof->guid);
                work.promote(prof->guid, /*needs_cap=*/true, &skipped);
                if (skipped) {
                    plan.notes.push_back("the Promote of " + prof->name + " is skipped: the profession is level " + std::to_string(before.level) + ", below its tier's cap " +
                                         std::to_string(ctx.professions->tier(before.tier).max_level) + " (the XP before a Promote brings it there)");
                }
            } else {
                work.demote(prof->guid);
            }
            continue;
        }
        if (action->response_type != "GiveItem") {
            return refuse("the action response " + action->name + " is of type " + action->response_type +
                          ", which stage 1 does not run (GiveItem, SetPrimaryArchetype and SetCertificationLevel are)");
        }
        for (const int64_t table_guid : action->reward_tables) {
            const auto table = db.reward_table(table_guid, error);
            if (!table) {
                if (!error.empty()) return refuse("content.db failed: " + error);
                return refuse(action->name + " names the reward table " + std::to_string(table_guid) + ", which content.db does not have");
            }
            if (!table->modelled) return refuse("the reward table " + table->name + " is not of the shape stage 1 grants (it does not grant always and select all)");
            if (table->level_below < 0) return refuse("the reward table " + table->name + " has a predicate stage 1 does not read: " + table->predicate);
            if (table->level_below > 0 && ctx.progress.level >= table->level_below) {
                return refuse("the reward table " + table->name + " is for a character below level " + std::to_string(table->level_below) +
                              " (the character is level " + std::to_string(ctx.progress.level) + "): nothing is granted and the item stays");
            }
            for (const content::RewardItem& r : table->items) {
                const auto rec = db.item(r.item, error);
                if (!rec) {
                    if (!error.empty()) return refuse("content.db failed: " + error);
                    return refuse("the reward table " + table->name + " grants item record " + std::to_string(r.item) + ", which content.db does not have");
                }
                const int32_t stack = rec->max_stack_size > 0 ? rec->max_stack_size : 1;
                int32_t left = r.quantity;
                while (left > 0) {
                    Granted g;
                    g.record = r.item;
                    g.quantity = left < stack ? left : stack;
                    g.quality = rec->default_quality;
                    g.durability = static_cast<float>(std::floor(rec->max_durability + 0.5));
                    plan.items.push_back(g);
                    left -= g.quantity;
                    if (plan.items.size() > kMaxGrantedItems) return refuse("the reward table " + table->name + " grants more than " + std::to_string(kMaxGrantedItems) + " stacks");
                }
                ++items_granted;
            }
            for (const content::RewardXp& x : table->experience) {
                if (x.experience_type == "Profession") {
                    if (x.expr <= 0) {
                        plan.notes.push_back("the Profession experience reward of " + table->name + " has no compiled expression: not granted");
                        continue;
                    }
                    if (!ctx.professions || !work.valid()) {
                        return refuse("the Profession experience reward of " + table->name + " needs the professions, and worldd has no profession tables (content.db is older than contentc 13?)");
                    }
                    const std::string named = professions::profession_named_in(x.expression);
                    const professions::Profession* prof = ctx.professions->named(named);
                    if (!prof) return refuse("the Profession experience reward of " + table->name + " names '" + named + "', which is no profession");
                    if (!ctx.profession_xp) {
                        return refuse("worldd has no expression runtime: the profession XP of " + table->name + " cannot be computed");
                    }
                    std::string why;
                    const auto value = ctx.profession_xp(x.expr, work.levels_by_name(), why);
                    if (!value) return refuse("the profession XP expression of " + table->name + " does not run: " + why);
                    const int64_t delta = *value >= 1.0 ? static_cast<int64_t>(std::min(std::floor(*value), 1.0e12)) : 0;
                    work.add_xp(prof->guid, delta);
                    xp_lines.push_back({prof, x.expr});
                    continue;
                }
                if (x.experience_type != "Adventuring") {
                    plan.notes.push_back("the " + x.experience_type + " experience reward of " + table->name + " is not granted (stage 1 levels Adventuring only)");
                    continue;
                }
                if (x.target_level == 0) {
                    plan.notes.push_back("the Adventuring experience reward of " + table->name + " is not the level scrolls' formula: not granted");
                    continue;
                }
                if (!ctx.curve) return refuse("worldd has no Adventuring level curve: the XP of " + table->name + " cannot be computed");
                const auto xp = leveling::scroll_xp(*ctx.curve, x.target_level, ctx.progress, ctx.xp_add_mult, x.margin);
                if (!xp) return refuse("the level " + std::to_string(x.target_level) + " of " + table->name + " is not on the Adventuring curve");
                plan.adventuring_xp += *xp;
            }
        }
    }
    // Gathering stage 2: the settlement. A profession that ends below the level its XP line asks (the data of `_All` has one XP line and one Promote per profession: the XP
    // stopped at the Novice cap and the Promote leaves level 25 out of reach) is promoted until its tier holds that level and given the XP of it, as the item's description
    // says: modelled. The level a line asks is the level a new profession reaches with it.
    if (work.valid()) {
        size_t settled = 0;
        std::set<int64_t> seen;
        for (const XpLine& line : xp_lines) {
            if (!seen.insert(line.profession->guid).second) continue;
            const professions::State s = work.state(line.profession->guid);
            std::map<std::string, int> fresh = work.levels_by_name();
            fresh[line.profession->name] = 1;
            std::string why;
            const auto value = ctx.profession_xp ? ctx.profession_xp(line.expr, fresh, why) : std::nullopt;
            if (!value || !(*value >= 1.0)) continue;
            const int target = ctx.professions->level_of(*line.profession, static_cast<int64_t>(std::min(std::floor(*value), 1.0e12)), professions::kTierGrandMaster);
            if (target <= s.level) continue;
            bool moved = false;
            while (work.tier(line.profession->guid) < professions::kTierGrandMaster &&
                   ctx.professions->tier(work.tier(line.profession->guid)).max_level < target) {
                work.promote(line.profession->guid, /*needs_cap=*/false);
                moved = true;
            }
            work.raise_xp_to(line.profession->guid, line.profession->curve.xp_for(target));
            settled += moved || work.level(line.profession->guid) > s.level;
        }
        if (settled > 0) {
            plan.notes.push_back("modelled: " + std::to_string(settled) + " profession(s) stopped at their tier's cap below the level the item's XP asks (its Promote actions are too few: `_All` promotes "
                                 "each profession once), so each was promoted until its tier holds that level, as the item's description says");
        }
        // only a profession that moved (a level or a tier) is changed: XP alone (a scroll's +1 past its level) is no change a player sees, and no reason to use a scroll
        professions::Professions kept = ctx.held;
        for (const professions::Change& c : professions::Professions::diff(ctx.held, work)) {
            if (c.moved()) kept.set_state(c.profession, c.after);
        }
        work = kept;
        plan.profession_changes = professions::Professions::diff(ctx.held, work);
        plan.professions_after = work;
        // a GM item ignores the certification's MaximumAllowable (a character may hold 5 Apprentice, 4 Journeyman ...)
        for (int t = 1; t < professions::kTiers; ++t) {
            const professions::Tier& tier = ctx.professions->tier(t);
            if (!tier.limited) continue;
            int at = 0;
            for (const professions::Profession& p : ctx.professions->all()) at += work.tier(p.guid) == t;
            if (at > tier.max_allowable) {
                plan.notes.push_back("a GM item ignores the certification's MaximumAllowable: " + std::to_string(at) + " professions are " + tier.name + " now (the limit is " +
                                     std::to_string(tier.max_allowable) + ")");
            }
        }
    }
    const bool professions_moved = !plan.profession_changes.empty();
    if (plan.items.empty() && plan.adventuring_xp == 0 && !plan.set_class && !professions_moved) {
        if (!xp_lines.empty()) {
            std::string who;
            for (const XpLine& line : xp_lines) {
                if (who.find(line.profession->name) != std::string::npos) continue;
                const professions::State s = ctx.held.state(line.profession->guid);
                who += (who.empty() ? "" : ", ") + line.profession->name + " is level " + std::to_string(s.level) + " (" + ctx.professions->tier(s.tier).name + ")";
            }
            return refuse("nothing would change: " + who + " already");
        }
        return refuse("the item's actions grant nothing stage 1 runs" + (plan.notes.empty() ? std::string() : ": " + plan.notes.front()));
    }
    if (professions_moved) {
        std::string line;
        if (plan.profession_changes.size() <= 3) {
            for (const professions::Change& c : plan.profession_changes) {
                const professions::Profession* p = ctx.professions->find(c.profession);
                line += (line.empty() ? "" : ", ") + (p ? p->name : std::to_string(c.profession)) + " level " + std::to_string(c.after.level) + " (" +
                        ctx.professions->tier(c.after.tier).name + ")";
            }
        } else {
            line = std::to_string(plan.profession_changes.size()) + " professions raised";
        }
        plan.summary += (plan.summary.empty() ? "" : ", ") + line;
    }
    if (plan.adventuring_xp > 0) plan.summary += (plan.summary.empty() ? "" : ", ") + std::string("+") + with_commas(plan.adventuring_xp) + " XP";
    if (!plan.items.empty()) {
        plan.summary += (plan.summary.empty() ? "" : ", ") + std::to_string(plan.items.size()) + " item(s)" +
                        (plan.items.size() != items_granted ? " in " + std::to_string(items_granted) + " reward(s)" : "");
    }
    plan.ok = true;
    return plan;
}

}  // namespace aoc::worldd::use

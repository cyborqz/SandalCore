// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/item_use.cpp (see item_use.h)
// ============================================================================
#include "session/item_use.h"

#include "bootstrap/character_setup.h"

#include <cmath>

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
        if (action->response_type != "GiveItem") {
            return refuse("the action response " + action->name + " is of type " + action->response_type + ", which stage 1 does not run (GiveItem and SetPrimaryArchetype are)");
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
    if (plan.items.empty() && plan.adventuring_xp == 0 && !plan.set_class) {
        return refuse("the item's actions grant nothing stage 1 runs" + (plan.notes.empty() ? std::string() : ": " + plan.notes.front()));
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

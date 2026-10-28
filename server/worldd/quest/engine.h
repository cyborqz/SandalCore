// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/engine.h  (quests milestone, slice 2, "do it and hand it in")
//
//  What happens to the quests in a character's log while the character plays: objectives wake up, become active, count, complete, and the quest completes when its objectives are done.
//  Pure logic over the log and the data: it changes the log and says, in a list of effects, what the world has to do (give items, take items, show a toast, give the rewards, take a
//  finished quest out of the log).  No network, no clock (the caller says what time it is).
//
//  How a quest runs (the game's own rules):
//    * An objective is Awake or not, Hidden or not, and Active or not.  An awake objective that is not active, not failed and not done becomes active as soon as its activation rule
//      holds; an action (ActivateObjective) activates one at once.  Only an active objective counts.
//    * Each kind has its own events (a conversation line, a kill, an interaction, a place, the items carried, a crate delivered); an event adds to the objective's count, which never
//      goes above the objective's CompletionCount.  An objective is done when its count has reached CompletionCount (a Custom objective: when its rule holds).
//    * A done objective runs its success actions.  The quest is done when every objective that is not optional is done, or when the objective that just finished completes the
//      quest by itself (bCompletesQuest).  A done quest runs its success actions and hands out the rewards of the quest and of every objective that was done.
//    * Actions (reveal, activate, succeed, fail, wake ...  an objective; set or raise a quest or player variable; give or take items; show a toast) run when an objective starts,
//      ticks, succeeds or fails, and when a quest starts, ticks, succeeds or fails.  An action may have a condition of its own.
//  The engine settles after every event: it goes on until nothing changes any more (the game does one step each tick; the result is the same).
// ============================================================================
#pragma once

#include "dialogue/requirement.h"
#include "quest/quest_book.h"
#include "quest/quest_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::quest {

/// What the world has to do because of what the engine did.
struct Effect {
    enum class Kind : uint8_t {
        GrantTable,     // give the reward table `tables[0]` (a GrantItem action, a GiveItems line)
        GrantRewards,   // `quest` is done: give these reward tables (`tables`)
        RemoveItems,    // take `items` (item record, quantity; 0: every one) out of the bags
        Toast,          // show `text`
        ObjectiveDone,  // an objective of `quest` was completed (`objective` is its record)
        QuestDone,      // `quest` is done: take it out of the log as finished
        QuestFailed,    // `quest` failed: take it out of the log
        StartQuest,     // start the quest `quest` (an ActivateQuest action)
        Told,           // `text`: something the data asked for that this milestone does not do (for the log, once)
    };
    Kind                                     kind = Kind::Told;
    int64_t                                  quest = 0, objective = 0;
    std::vector<int64_t>                     tables;
    std::vector<std::pair<int64_t, int64_t>> items;
    std::string                              text;
};

/// The node a conversation with the NPC `npc` (its population instance) starts at because of an active Dialogue objective that names it and a node (0: no such objective).  The
/// first one in the log's order wins.
inline int64_t dialogue_entry(const QuestLog& log, const QuestBook& book, int64_t npc) {
    if (npc == 0) return 0;
    for (const QuestInstance& q : log.instances()) {
        if (q.status != Status::Active) continue;
        const content::QuestRow* d = book.quest(q.record);
        if (!d) continue;
        for (size_t i = 0; i < q.objectives.size() && i < d->objectives.size(); ++i) {
            const ObjectiveState& o = q.objectives[i];
            const content::QuestObjectiveRow& r = d->objectives[i];
            if (!o.active || o.completed || o.failed || r.type != "Dialogue") continue;
            for (const content::QuestSource& s : r.sources) {
                if ((s.kind.empty() || s.kind == "NPC") && s.instance == npc && s.node != 0) return s.node;
            }
        }
    }
    return 0;
}

class Engine {
public:
    /// `ev` answers the rules (its Facts should be `facts`: the objective counts of the items carried come from them); `effects` collects what the world has to do.
    Engine(QuestLog& log, const QuestBook& book, dialogue::Evaluator& ev, const dialogue::Facts& facts, std::vector<Effect>& effects, double now)
        : log_(log), book_(book), ev_(ev), facts_(facts), fx_(effects), now_(now) {}

    // ---- starting -------------------------------------------------------------------------------------------------------------------------------------------------

    /// Takes the quest (the caller has asked QuestLog::can_start): the instance joins the log, its start actions run and everything settles.  The instance, or null when the log has none.
    QuestInstance* start(const content::QuestRow& def, int64_t source) {
        Started s = log_.start(def, now_, source, /*run_actions=*/false);
        QuestInstance* q = s.instance;
        if (!q) return nullptr;
        const int64_t record = q->record;
        run_actions(def.start_actions, *q, -1);
        settle();
        return log_.find(record);
    }

    /// Goes through every quest until nothing changes any more.  True when anything did.
    bool settle() {
        bool any = false;
        for (int pass = 0; pass < kMaxPasses; ++pass) {
            bool changed = false;
            for (QuestInstance& q : log_.instances_for_wire()) {
                if (q.status != Status::Active) continue;
                const content::QuestRow* d = book_.quest(q.record);
                if (d) changed = tick_quest(q, *d) || changed;
            }
            any = any || changed;
            if (!changed) break;
        }
        return any;
    }

    // ---- events ---------------------------------------------------------------------------------------------------------------------------------------------------

    /// A dialogue action CompleteDialogueObjective (`complete`) or IncrementDialogueObjective ran in a conversation with the NPC `npc` (its population instance) that has visited the
    /// nodes `visited`: every active Dialogue objective that names this NPC and one of those nodes counts.
    bool dialogue(int64_t npc, const std::set<int64_t>& visited, bool complete) {
        bool any = false;
        each_active("Dialogue", [&](QuestInstance& q, size_t i, const content::QuestObjectiveRow& r) {
            bool match = false;
            for (const content::QuestSource& s : r.sources) {
                if ((!s.kind.empty() && s.kind != "NPC") || s.instance == 0 || s.instance != npc) continue;
                if (s.node == 0 || visited.count(s.node)) match = true;
            }
            if (!match) return;
            update_objective(q, i, r, complete ? r.count - q.objectives[i].count : 1);
            any = true;
        });
        return finish_event(any);
    }

    /// The character killed the creature `instance` (a population instance) of elite rank `elite`.
    bool kill(int64_t instance, int elite) {
        bool any = false;
        const std::vector<int64_t>& sets = book_.kill_sets(instance);
        // the quest items the creature drops for an active objective of the character's quests (before the kill counts: the kill may finish the objective)
        for (QuestInstance& q : log_.instances_for_wire()) {
            if (q.status != Status::Active) continue;
            const content::QuestRow* d = book_.quest(q.record);
            if (!d) continue;
            for (size_t i = 0; i < q.objectives.size() && i < d->objectives.size(); ++i) {
                const ObjectiveState& o = q.objectives[i];
                if (!o.active || o.completed || o.failed) continue;
                for (const content::QuestObjectiveRow::ItemReward& ir : d->objectives[i].item_rewards) {
                    bool match = std::find(ir.instances.begin(), ir.instances.end(), instance) != ir.instances.end();
                    for (size_t k = 0; k < sets.size() && !match; ++k) match = std::find(ir.sets.begin(), ir.sets.end(), sets[k]) != ir.sets.end();
                    if (!match || ir.table == 0) continue;
                    Effect e;
                    e.kind = Effect::Kind::GrantTable;
                    e.quest = q.record;
                    e.tables.push_back(ir.table);
                    fx_.push_back(std::move(e));
                    any = true;
                }
            }
        }
        each_active("Kill", [&](QuestInstance& q, size_t i, const content::QuestObjectiveRow& r) {
            if (r.kill_owner || r.use_context) return;   // quest-owner kills and quest-spawned creatures are not modelled
            if (r.advanced && elite < r.elite) return;
            bool match = std::find(r.kill_instances.begin(), r.kill_instances.end(), instance) != r.kill_instances.end();
            for (size_t k = 0; k < sets.size() && !match; ++k) match = std::find(r.kill_sets.begin(), r.kill_sets.end(), sets[k]) != r.kill_sets.end();
            if (!match) return;
            update_objective(q, i, r, 1);
            any = true;
        });
        return finish_event(any);
    }

    /// The character used an interactable made from the component definition `component` (and the actor definition `actor`; 0: none).
    bool interact(int64_t component, int64_t actor) {
        bool any = false;
        each_active("Interact", [&](QuestInstance& q, size_t i, const content::QuestObjectiveRow& r) {
            const bool match = (component != 0 && std::find(r.components.begin(), r.components.end(), component) != r.components.end()) ||
                               (actor != 0 && std::find(r.actors.begin(), r.actors.end(), actor) != r.actors.end());
            if (!match) return;
            update_objective(q, i, r, 1);
            any = true;
        });
        return finish_event(any);
    }

    /// The character stands at (`x`, `y`, `z`) cm: an active GoTo objective with a place that completes it, and the character inside it, is done.
    bool position(double x, double y, double z) {
        bool any = false;
        each_active("GoTo", [&](QuestInstance& q, size_t i, const content::QuestObjectiveRow& r) {
            for (const content::QuestLocation& l : r.locations) {
                if (!l.completes) continue;
                const double dx = x - l.x, dy = y - l.y, dz = z - l.z;
                if (std::sqrt(dx * dx + dy * dy) > l.radius || std::fabs(dz) > std::max(l.radius, 2000.0)) continue;
                update_objective(q, i, r, r.count - q.objectives[i].count);
                any = true;
                break;
            }
        });
        return finish_event(any);
    }

    /// Whether any active objective is a GoTo one (so that the caller asks for the position only when it matters).
    bool wants_position() {
        bool any = false;
        each_active("GoTo", [&](QuestInstance&, size_t, const content::QuestObjectiveRow&) { any = true; });
        return any;
    }

    /// `count` crates were put into a construction project: `item` is the crate item, `crate_type` its crate type (0: unknown).  Every active TurnIn objective that takes crates of that
    /// type (or that item) counts them.  Returns how many crates the quests took (0: none counts).
    int64_t deposit(int64_t item, int64_t crate_type, int64_t count) {
        if (count <= 0) return 0;
        int64_t taken = 0;
        each_active("TurnIn", [&](QuestInstance& q, size_t i, const content::QuestObjectiveRow& r) {
            if (!r.crate) return;
            const bool match = (crate_type != 0 && std::find(r.crate_types.begin(), r.crate_types.end(), crate_type) != r.crate_types.end()) ||
                               (item != 0 && std::find(r.crate_items.begin(), r.crate_items.end(), item) != r.crate_items.end());
            if (!match) return;
            const int64_t before = q.objectives[i].count;
            update_objective(q, i, r, count);
            taken = std::max(taken, q.objectives[i].count - before);
            if (r.inc_variable && !r.quest_variable.empty()) log_.set_quest_variable(q.record, r.quest_variable, log_.quest_variable(q.record, r.quest_variable) + count);
        });
        finish_event(taken > 0);
        return taken;
    }

    /// A player event of the game's notification kinds happened (an EventNotification objective names the kind and, for a crafting event, the recipe): ConstructionCrateEquipped,
    /// ContributedToServiceBuildingConstruction, OpenedCrafting ...  `recipe` 0: none.
    bool notified(const std::string& type, int64_t recipe) {
        bool any = false;
        each_active("EventNotification", [&](QuestInstance& q, size_t i, const content::QuestObjectiveRow& r) {
            if (r.notify != type) return;
            if (r.recipe != 0 && r.recipe != recipe) return;
            update_objective(q, i, r, 1);
            any = true;
        });
        return finish_event(any);
    }

    /// The character collected `count` of the item `item` in the way `how` (Purchase, Sell, Craft, Process, Equip): every active Collect objective of that kind that names the item counts.
    bool collected(const std::string& how, int64_t item, int64_t count) {
        bool any = false;
        each_active("Collect", [&](QuestInstance& q, size_t i, const content::QuestObjectiveRow& r) {
            if (r.collect_type != how || std::find(r.items.begin(), r.items.end(), item) == r.items.end()) return;
            update_objective(q, i, r, count);
            any = true;
        });
        return finish_event(any);
    }

    /// The character used the ability `ability`.
    bool ability(int64_t ability_id) {
        bool any = false;
        each_active("UseAbility", [&](QuestInstance& q, size_t i, const content::QuestObjectiveRow& r) {
            if (std::find(r.abilities.begin(), r.abilities.end(), ability_id) == r.abilities.end()) return;
            update_objective(q, i, r, 1);
            any = true;
        });
        return finish_event(any);
    }

    // ---- actions of a dialogue (the game runs them as the quest's own) -------------------------------------------------------------------------------------------------

    /// One action of a conversation line or node: ActivateObjective, RevealObjective or FailObjective on the quest and objective it names, GiveItems (reward tables).  Anything else is not the
    /// engine's.  True when something changed.
    bool dialogue_action(const content::DialogueActionRow& a) {
        if (a.action == "GiveItems") {
            for (const int64_t table : a.rewards) {
                Effect e;
                e.kind = Effect::Kind::GrantTable;
                e.tables.push_back(table);
                fx_.push_back(std::move(e));
            }
            return !a.rewards.empty();
        }
        if (a.action != "ActivateObjective" && a.action != "RevealObjective" && a.action != "FailObjective") return false;
        if (a.quest == 0 || a.objective == 0) return false;
        QuestInstance* q = log_.find(a.quest);
        if (!q || q->status != Status::Active) return false;
        content::QuestAction act;
        act.action = a.action;
        act.quest = a.quest;
        act.objective = a.objective;
        const bool changed = apply(act, *q, -1);
        settle();
        return changed;
    }

private:
    static constexpr int kMaxPasses = 12;

    QuestLog&                 log_;
    const QuestBook&          book_;
    dialogue::Evaluator&      ev_;
    const dialogue::Facts&    facts_;
    std::vector<Effect>&      fx_;
    double                    now_;
    std::set<std::string>     told_;

    bool finish_event(bool any) {
        if (any) settle();
        return any;
    }

    /// Calls `fn(quest, index, record)` for every active, unfinished objective of the given kind in every active quest.  (`fn` may change the log's values, never the vector's size.)
    template <typename Fn>
    void each_active(const char* type, Fn&& fn) {
        for (QuestInstance& q : log_.instances_for_wire()) {
            if (q.status != Status::Active) continue;
            const content::QuestRow* d = book_.quest(q.record);
            if (!d) continue;
            for (size_t i = 0; i < q.objectives.size() && i < d->objectives.size(); ++i) {
                const ObjectiveState& o = q.objectives[i];
                if (!o.active || o.completed || o.failed) continue;
                if (d->objectives[i].type != type) continue;
                fn(q, i, d->objectives[i]);
            }
        }
    }

    void told(const std::string& what) {
        if (!told_.insert(what).second) return;
        Effect e;
        e.kind = Effect::Kind::Told;
        e.text = what;
        fx_.push_back(std::move(e));
    }

    static int index_of(const QuestInstance& q, int64_t objective) {
        for (size_t i = 0; i < q.objectives.size(); ++i) {
            if (q.objectives[i].record == objective) return static_cast<int>(i);
        }
        return -1;
    }

    /// Adds `delta` to an objective's count; the count never goes above the objective's CompletionCount.
    void update_objective(QuestInstance& q, size_t i, const content::QuestObjectiveRow& r, int64_t delta) {
        ObjectiveState& o = q.objectives[i];
        const int64_t top = std::max<int64_t>(r.count, 0);
        const int64_t next = std::min<int64_t>(std::max<int64_t>(o.count + delta, 0), top);
        if (next == o.count) return;
        o.count = static_cast<int32_t>(next);
        log_.touch(q);
    }

    // ---- the objectives -------------------------------------------------------------------------------------------------------------------------------------------

    void activate(QuestInstance& q, size_t i) {
        ObjectiveState& o = q.objectives[i];
        o.active = true;
        o.awake = true;   // (as the first slice modelled it: an objective an action activates is awake, as the quest's own first one is)
        o.succeeded = o.completed = o.failed = false;
        o.start_time = now_;
        log_.touch(q);
        const content::QuestRow* d = book_.quest(q.record);
        if (d && i < d->objectives.size()) run_actions(d->objectives[i].on_start, q, static_cast<int>(i));
    }

    bool all_required_done(const QuestInstance& q, const content::QuestRow& d) const {
        for (size_t i = 0; i < q.objectives.size(); ++i) {
            const bool optional = i < d.objectives.size() && d.objectives[i].optional;
            if (!optional && !q.objectives[i].completed) return false;
        }
        return true;
    }

    /// Completes an active objective (the game's CompleteObjective); the quest is done when every required objective is, or this one completes the quest.
    bool complete_objective(QuestInstance& q, size_t i) {
        ObjectiveState& o = q.objectives[i];
        if (o.completed || o.failed || !o.active) return false;
        o.active = false;
        o.succeeded = true;
        o.completed = true;
        log_.touch(q);
        Effect e;
        e.kind = Effect::Kind::ObjectiveDone;
        e.quest = q.record;
        e.objective = o.record;
        fx_.push_back(std::move(e));
        const content::QuestRow* d = book_.quest(q.record);
        if (!d) return true;
        const bool completes = i < d->objectives.size() && d->objectives[i].completes;
        if (i < d->objectives.size()) run_actions(d->objectives[i].on_success, q, static_cast<int>(i));
        if (q.status == Status::Active && (all_required_done(q, *d) || completes)) complete_quest(q);
        return true;
    }

    bool fail_objective(QuestInstance& q, size_t i) {
        ObjectiveState& o = q.objectives[i];
        if (o.failed || o.completed) return false;
        o.failed = true;
        o.active = false;
        log_.touch(q);
        const content::QuestRow* d = book_.quest(q.record);
        if (d && i < d->objectives.size()) run_actions(d->objectives[i].on_fail, q, static_cast<int>(i));
        return true;
    }

    bool complete_quest(QuestInstance& q) {
        if (q.status != Status::Active) return false;
        q.status = Status::Completed;
        q.end_time = now_;
        log_.touch(q);
        const content::QuestRow* d = book_.quest(q.record);
        if (d) run_actions(d->on_success, q, -1);
        Effect rewards;
        rewards.kind = Effect::Kind::GrantRewards;
        rewards.quest = q.record;
        if (d) {
            rewards.tables = d->rewards;
            for (size_t i = 0; i < q.objectives.size() && i < d->objectives.size(); ++i) {
                if (!q.objectives[i].completed) continue;
                rewards.tables.insert(rewards.tables.end(), d->objectives[i].rewards.begin(), d->objectives[i].rewards.end());
            }
        }
        if (!rewards.tables.empty()) fx_.push_back(std::move(rewards));
        Effect done;
        done.kind = Effect::Kind::QuestDone;
        done.quest = q.record;
        fx_.push_back(std::move(done));
        return true;
    }

    bool fail_quest(QuestInstance& q) {
        if (q.status != Status::Active) return false;
        q.status = Status::Failed;
        q.end_time = now_;
        log_.touch(q);
        const content::QuestRow* d = book_.quest(q.record);
        if (d) run_actions(d->on_fail, q, -1);
        Effect e;
        e.kind = Effect::Kind::QuestFailed;
        e.quest = q.record;
        fx_.push_back(std::move(e));
        return true;
    }

    // ---- the actions ----------------------------------------------------------------------------------------------------------------------------------------------

    /// Runs a list of actions of quest `q` (of its objective `self`, or -1 for the quest's own list).  True when any of them changed something.
    bool run_actions(const std::vector<content::QuestAction>& list, QuestInstance& q, int self) {
        bool any = false;
        for (const content::QuestAction& a : list) {
            if (!a.rule.empty()) {
                ev_.scope(q.record);
                const bool ok = ev_.holds(a.rule);
                ev_.scope(0);
                if (!ok) continue;
            }
            any = apply(a, q, self) || any;
        }
        return any;
    }

    bool apply(const content::QuestAction& a, QuestInstance& q, int self) {
        QuestInstance* tq = &q;
        if (a.quest != 0 && a.quest != q.record) tq = log_.find(a.quest);   // an action may name another quest the character has
        int target = -1;
        if (tq) target = a.objective != 0 ? index_of(*tq, a.objective) : (tq == &q ? self : -1);
        const std::string& n = a.action;
        if (n == "RevealObjective" || n == "HideObjective" || n == "WakeObjective" || n == "ActivateObjective" || n == "DeactivateObjective" || n == "SucceedObjective" ||
            n == "FailObjective" || n == "ResetObjective") {
            if (!tq || target < 0) return false;
            ObjectiveState& o = tq->objectives[static_cast<size_t>(target)];
            const size_t i = static_cast<size_t>(target);
            if (n == "RevealObjective") {
                if (!o.hidden) return false;
                o.hidden = false;
            } else if (n == "HideObjective") {
                if (o.hidden) return false;
                o.hidden = true;
            } else if (n == "WakeObjective") {
                if (o.awake) return false;
                o.awake = true;
            } else if (n == "ActivateObjective") {
                if (o.completed) return false;   // an objective that is done stays done (a conversation node that is entered again must not open it again; ResetObjective does)
                activate(*tq, i);
                return true;
            } else if (n == "DeactivateObjective") {
                if (!o.active) return false;
                o.active = false;
            } else if (n == "SucceedObjective") {
                return complete_objective(*tq, i);
            } else if (n == "FailObjective") {
                return fail_objective(*tq, i);
            } else {   // ResetObjective
                o.active = o.succeeded = o.completed = o.failed = false;
                o.count = 0;
            }
            log_.touch(*tq);
            return true;
        }
        if (n == "CompleteQuest") return tq && complete_quest(*tq);
        if (n == "FailQuest") return tq && fail_quest(*tq);
        if (n == "ActivateQuest") {
            if (a.quest == 0) return false;
            Effect e;
            e.kind = Effect::Kind::StartQuest;
            e.quest = a.quest;
            fx_.push_back(std::move(e));
            return true;
        }
        if (n == "IncrementQuestVariable") {
            log_.set_quest_variable(q.record, a.variable, log_.quest_variable(q.record, a.variable) + a.value);
            return true;
        }
        if (n == "SetQuestVariable") {
            log_.set_quest_variable(q.record, a.variable, a.value);
            return true;
        }
        if (n == "SetPlayerVariable") {
            log_.set_variable(a.variable, a.value);
            return true;
        }
        if (n == "IncrementPlayerVariable") {
            log_.set_variable(a.variable, log_.variable(a.variable) + a.value);
            return true;
        }
        if (n == "RemovePlayerVariable") {
            log_.set_variable(a.variable, 0);
            return true;
        }
        if (n == "GrantItem") {
            if (a.reward_table == 0) return false;
            Effect e;
            e.kind = Effect::Kind::GrantTable;
            e.quest = q.record;
            e.tables.push_back(a.reward_table);
            fx_.push_back(std::move(e));
            return true;
        }
        if (n == "RemoveItem") {
            if (a.items.empty()) return false;
            Effect e;
            e.kind = Effect::Kind::RemoveItems;
            e.items = a.items;
            fx_.push_back(std::move(e));
            return true;
        }
        if (n == "Toast") {
            if (a.text.empty()) return false;
            Effect e;
            e.kind = Effect::Kind::Toast;
            e.text = a.text;
            fx_.push_back(std::move(e));
            return false;
        }
        // the world's own switches (spawns, weather, effects, foliage ...) and the variables of the world: not done by this milestone
        told("the quest action " + n + " is not done yet");
        return false;
    }

    // ---- the tick -------------------------------------------------------------------------------------------------------------------------------------------------

    /// The value a Custom objective's rule has: the quest variable it reads (a counter), else 1 when the rule holds and 0 when not.
    int64_t custom_value(QuestInstance& q, const content::QuestObjectiveRow& r) {
        if (r.rule.criteria.size() == 1 && r.rule.criteria[0].kind == "LocalQuestVariable") return std::max<int64_t>(0, facts_.quest_variable(q.record, r.rule.criteria[0].variable));
        ev_.scope(q.record);
        const bool ok = ev_.holds(r.rule);
        ev_.scope(0);
        return ok ? std::max<int64_t>(r.count, 1) : 0;
    }

    bool custom_holds(QuestInstance& q, const content::QuestObjectiveRow& r) {
        if (r.rule.empty()) return false;
        ev_.scope(q.record);
        const bool ok = ev_.holds(r.rule);
        ev_.scope(0);
        return ok;
    }

    bool tick_quest(QuestInstance& q, const content::QuestRow& d) {
        bool any = run_actions(d.on_tick, q, -1);
        if (q.status != Status::Active) return true;
        if (d.timeout > 0 && now_ >= q.start_time + d.timeout) {
            if (d.succeed_on_timeout) complete_quest(q);
            else fail_quest(q);
            return true;
        }
        for (size_t i = 0; i < q.objectives.size() && i < d.objectives.size(); ++i) {
            if (q.status != Status::Active) return true;
            ObjectiveState& o = q.objectives[i];
            const content::QuestObjectiveRow& r = d.objectives[i];
            if (!o.active) {
                if (o.awake && !o.failed && !o.completed) {
                    ev_.scope(q.record);
                    const bool ok = ev_.holds(r.activation);
                    ev_.scope(0);
                    if (ok) {
                        activate(q, i);
                        any = true;
                    }
                }
                continue;
            }
            if (run_actions(r.on_tick, q, static_cast<int>(i))) any = true;
            if (q.status != Status::Active) return true;
            if (r.timeout > 0 && now_ >= o.start_time + r.timeout) {
                any = (r.succeed_on_timeout ? complete_objective(q, i) : fail_objective(q, i)) || any;
                continue;
            }
            bool done;
            if (r.type == "Custom") {
                const int64_t value = std::min<int64_t>(custom_value(q, r), std::max<int64_t>(r.count, 1));
                if (value != o.count) {
                    o.count = static_cast<int32_t>(value);
                    log_.touch(q);
                    any = true;
                }
                done = custom_holds(q, r);
            } else {
                if (r.type == "Collect" && (r.collect_type.empty() || r.collect_type == "InventoryUpdate") && (!r.items.empty() || !r.tags.empty())) {
                    // the count follows the items carried (the bag), up to what the objective asks for
                    const int64_t have = std::min<int64_t>(facts_.item_count(r.items, r.items.empty() ? r.tags : std::vector<std::string>()), std::max<int64_t>(r.count, 0));
                    if (have != o.count) {
                        o.count = static_cast<int32_t>(have);
                        log_.touch(q);
                        any = true;
                    }
                }
                if (r.type == "Kill" && r.tag_query && r.kill_sets.empty() && r.kill_instances.empty()) told("a kill objective by tag query is not done yet");
                done = o.count >= r.count;
            }
            if (done && complete_objective(q, i)) any = true;
        }
        return any;
    }
};

}  // namespace aoc::worldd::quest

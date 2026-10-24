// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  dialogue/requirement.h
//
//  Conditions of a conversation: whether a line is shown, which line an automatic node takes, whether a quest is available.
//
//  A condition is a tree over criteria (content::DialogueRequirement): AND and OR join the groups below them, ALL / ANY / NONE say how many of a group's
//  criteria must hold (every one, at least one, none).  A criterion compares a number the character has with a number the data names:
//
//    QuestState      Quest(X.Available | Active | Completed | ReadyForHandIn)
//    ObjectiveState  Objective(X.Y.Active | Succeeded | Hidden | Completed | cCount)
//    Item            how many of the named items (or tags) the character carries
//    Currency        how much of a currency the character has
//    PlayerVariable / WorldVariable    the named variable
//    Expression      the value of an expression (expression.h)
//
//  The character has no quests yet (this milestone saves none): a quest is never active, completed or ready for hand-in, an objective is in no state, and a
//  quest is Available when it is not disabled and its own activation rule holds (the same evaluation, one level down).  Anything the evaluator cannot answer -- a
//  function of an expression nobody provides, a criterion of another kind, a tree that does not read -- makes the criterion (the tree) not hold, and is told to
//  the caller once.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "dialogue/expression.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::dialogue {

/// What the evaluator asks of the character who talks and of the world around: every default says "nothing" / "unknown".
class Facts : public ExprHost {
public:
    /// How many of the named item records (or, when `records` is empty, the item tags) the character carries.
    virtual int64_t item_count(const std::vector<int64_t>& /*records*/, const std::vector<std::string>& /*tags*/) const { return 0; }
    virtual int64_t currency(int64_t /*def*/) const { return 0; }
    virtual int64_t player_variable(const std::string& /*name*/) const { return 0; }
    virtual int64_t world_variable(const std::string& /*name*/) const { return 0; }
    std::optional<ExprValue> call(const std::string& /*name*/, const std::vector<ExprValue>& /*args*/) const override { return std::nullopt; }
    int64_t random_range(int64_t lo, int64_t hi) const override {
        if (hi <= lo) return lo;
        rng_ ^= rng_ << 13;   // xorshift
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return lo + static_cast<int64_t>(rng_ % static_cast<uint64_t>(hi - lo + 1));
    }

    mutable uint64_t rng_ = 88172645463325252ull;   // the dice's state; a test sets it
};

/// The conversations of the data, read whole.
class Book {
public:
    Book() = default;
    explicit Book(content::DialogueData data) : d_(std::move(data)) {}

    const content::DialogueNodeRow* node(int64_t guid) const {
        const auto it = d_.nodes.find(guid);
        return it == d_.nodes.end() ? nullptr : &it->second;
    }
    /// The node a root starts at; 0 for a root without a conversation or one the data does not hold.
    int64_t top_node(int64_t root) const {
        const auto it = d_.roots.find(root);
        return it == d_.roots.end() ? 0 : it->second;
    }
    bool has_root(int64_t root) const { return d_.roots.count(root) != 0; }
    const content::DialogueQuestRow* quest(int64_t guid) const {
        const auto it = d_.quests.find(guid);
        return it == d_.quests.end() ? nullptr : &it->second;
    }
    /// The shops (VendorDefs) that a line of the conversation of `root` opens, wherever in the conversation the line is: what a client that opens a shop from the window may name.
    std::set<int64_t> vendor_defs(int64_t root) const {
        std::set<int64_t> shops;
        std::set<int64_t> seen;
        std::vector<int64_t> todo;
        if (const int64_t top = top_node(root)) todo.push_back(top);
        while (!todo.empty()) {
            const int64_t guid = todo.back();
            todo.pop_back();
            if (!seen.insert(guid).second) continue;
            const content::DialogueNodeRow* n = node(guid);
            if (!n) continue;
            const auto take = [&](const std::vector<content::DialogueActionRow>& actions) {
                for (const content::DialogueActionRow& a : actions) {
                    if (a.action == "OpenWindow" && a.window == "Vendor" && a.vendor != 0) shops.insert(a.vendor);
                }
            };
            take(n->on_enter);
            take(n->on_exit);
            for (const content::DialogueChoiceRow& c : n->choices) {
                take(c.actions);
                if (c.next != 0) todo.push_back(c.next);
            }
        }
        return shops;
    }
    size_t nodes() const { return d_.nodes.size(); }
    size_t roots() const { return d_.roots.size(); }
    size_t quests() const { return d_.quests.size(); }
    const content::DialogueData& data() const { return d_; }

private:
    content::DialogueData d_;
};

/// Evaluates conditions for one character.  `unsupported` (optional) is told, once for each text, of everything the evaluator could not answer.
class Evaluator {
public:
    using Report = std::function<void(const std::string&)>;

    Evaluator(const Book& book, const Facts& facts, Report unsupported = nullptr) : book_(book), facts_(facts), report_(std::move(unsupported)) {}

    /// True when the condition holds; an empty condition always does.
    bool holds(const content::DialogueRequirement& r) { return r.empty() ? true : tree(r, 0); }

    /// Quest(X.Available): not disabled, not done (a repeatable quest may be done again), and the quest's activation rule holds.
    bool quest_available(int64_t quest_guid) { return available(quest_guid, 0); }

private:
    static constexpr int kMaxDepth = 8;

    void unsupported(const std::string& what) {
        if (!report_) return;
        if (told_.insert(what).second) report_(what);
    }

    bool tree(const content::DialogueRequirement& r, int depth) {
        if (depth > kMaxDepth) { unsupported("a condition nests too deep"); return false; }
        std::vector<std::optional<bool>> memo(r.criteria.size());
        size_t pos = 0;
        const auto ok = node(r, memo, pos, depth);
        if (!ok || pos != r.tokens.size()) { unsupported("a condition whose tree does not read"); return false; }
        return *ok;
    }

    std::optional<bool> node(const content::DialogueRequirement& r, std::vector<std::optional<bool>>& memo, size_t& pos, int depth) {
        if (pos + 2 > r.tokens.size()) return std::nullopt;
        const int type = r.tokens[pos], count = r.tokens[pos + 1];
        pos += 2;
        if (count < 0) return std::nullopt;
        if (type == 0 || type == 2) {   // AND, OR over nodes (every child is read, so the tree always ends where it should)
            bool all = true, any = false;
            for (int i = 0; i < count; ++i) {
                const auto c = node(r, memo, pos, depth);
                if (!c) return std::nullopt;
                all = all && *c;
                any = any || *c;
            }
            return type == 0 ? all : any;
        }
        if (type == 1 || type == 3 || type == 5) {   // ALL, ANY, NONE over criteria
            int holding = 0;
            for (int i = 0; i < count; ++i) {
                if (pos >= r.tokens.size()) return std::nullopt;
                const int index = r.tokens[pos++];
                if (index < 0 || static_cast<size_t>(index) >= r.criteria.size()) return std::nullopt;
                if (!memo[static_cast<size_t>(index)]) memo[static_cast<size_t>(index)] = criterion(r.criteria[static_cast<size_t>(index)], depth);
                if (*memo[static_cast<size_t>(index)]) ++holding;
            }
            if (type == 1) return holding == count;
            if (type == 3) return holding > 0;
            return holding == 0;
        }
        return std::nullopt;
    }

    static std::optional<bool> compare(const std::string& op, int64_t left, int64_t right) {
        if (op == "Equal") return left == right;
        if (op == "NotEqual") return left != right;
        if (op == "GreaterThan") return left > right;
        if (op == "GreaterThanOrEqual") return left >= right;
        if (op == "LessThan") return left < right;
        if (op == "LessThanOrEqual") return left <= right;
        return std::nullopt;
    }

    bool criterion(const content::DialogueCriterion& c, int depth) {
        const auto left = measure(c, depth);
        if (!left) return false;
        int64_t right = c.value;
        if (!c.right_variable.empty()) right = facts_.player_variable(c.right_variable);
        const auto r = compare(c.op, *left, right);
        if (!r) { unsupported("the comparison " + c.op); return false; }
        return *r;
    }

    std::optional<int64_t> measure(const content::DialogueCriterion& c, int depth) {
        if (c.kind == "QuestState") {
            if (c.quest_query == "Available") return available(c.quest, depth + 1) ? 1 : 0;
            if (c.quest_query == "Active" || c.quest_query == "Completed" || c.quest_query == "ReadyForHandIn") return 0;   // no quest log yet
            unsupported("the quest query " + c.quest_query);
            return std::nullopt;
        }
        if (c.kind == "ObjectiveState") return 0;   // an objective of a quest nobody has is in no state
        if (c.kind == "Item") return facts_.item_count(c.items, c.tags);
        if (c.kind == "Currency") return facts_.currency(c.currency);
        if (c.kind == "PlayerVariable") return facts_.player_variable(c.variable);
        if (c.kind == "WorldVariable") return facts_.world_variable(c.variable);
        if (c.kind == "Expression") {
            const auto v = evaluate_expression(c.expression, facts_);
            if (!v) { unsupported("the expression " + c.expression); return std::nullopt; }
            return v->i;
        }
        unsupported("the criterion kind " + c.kind);
        return std::nullopt;
    }

    bool available(int64_t guid, int depth) {
        if (depth > kMaxDepth || guid == 0) return false;
        const content::DialogueQuestRow* q = book_.quest(guid);
        if (!q || q->disabled) return false;
        if (!resolving_.insert(guid).second) return false;   // a rule that leads back to itself
        const bool ok = q->activation.empty() ? true : tree(q->activation, depth);
        resolving_.erase(guid);
        return ok;   // nothing is completed yet, so a quest that was not repeatable is still open
    }

    const Book&          book_;
    const Facts&         facts_;
    Report               report_;
    std::set<std::string> told_;
    std::set<int64_t>    resolving_;
};

}  // namespace aoc::worldd::dialogue

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  dialogue/talk.h
//
//  One player's conversation with one NPC, as the talking window shows it.  Pure logic over the Book: no network, no clock.  The caller (the session)
//  turns the state into the PlayerController's CurrentDialogueInstance and sends what changed.
//
//  The window has these states, and the client names every move by the node it shows and the index of the line in that node's whole list:
//
//    greeting    after the click: the node the NPC's root leads to (an automatic node first takes its first line whose condition holds, and
//                so on to a node with words), its lines whose conditions hold, and nothing of "where we came from" (no CurrentNode: the greeting is
//                not a step).  An NPC that offers a quest starts at the node its offer names while the quest can be taken, else at its root
//    step        after the player chose a line that leads on: PreviousNode is the node the player was in (none from the greeting), PreviousSelectedChoice the line
//                (its node and index), CurrentNode and the shown node the node it led to.  A line that leads to an automatic node passes through it: the automatic node is
//                the previous node and its chosen line the previous choice
//    offer       a step (or a greeting) that arrives at a node that offers a quest: the node is entered like any other and the quest panel is queued on top of it
//                (`sub`).  The player answers the panel (Accept, Decline or Close); an accepted quest goes on to the node the offer names (none: the talk ends), a declined one ends the talk
//    cleared     the line ends the conversation, the client closed the window, the player went away or died: every field back to nothing
//
//  What this milestone does not do (it opens no windows but a shop's and gives nothing): a line's actions are skipped and reported; a line that ends the
//  conversation but has actions changes nothing (the window stays); a node of a kind that needs hand-ins or choices (TurnIn, Confirmation, BuyOrderFulfillment)
//  is never shown (the line that leads there changes nothing).  ExhaustBehavior is CloseAndClear for all but one node of the data, and every node is treated so.
//  A node with no line that shows is not shown either (the game's own conversations never showed one): a click on an NPC that starts at one does nothing, and a line that leads to
//  one ends the conversation.
// ============================================================================
#pragma once

#include "dialogue/requirement.h"
#include "protocol/repl/dialogue_rpc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::dialogue {

/// The most automatic nodes one step may pass through (the data's longest chain is 6).
inline constexpr int kMaxAutomaticSteps = 8;

enum class Outcome { Changed, Nothing, Refused };

struct Result {
    Outcome                  outcome = Outcome::Nothing;
    std::string              why;       // for the log: why nothing changed, or what was skipped
    std::vector<std::string> skipped;   // actions of a line or node that were not done ("GiveItems")
    int64_t                  open_vendor = 0;   // the line opens this shop (a VendorDef): the caller opens its window; the conversation itself is left as it is
    bool                     offer = false;     // the conversation now shows a quest offer (state().sub)
    bool                     offer_answered = false;   // the quest offer was answered or refused: the update carries the cleared queue and ExhaustBehavior
    int                      open_warehouse = 0;   // storage, step 0: the line opens the character's warehouse (the action response OpenPlayerWarehouse): its StorageAccessConfig; the conversation ends
};

class Talk {
public:
    /// One line of the window: the node it belongs to and its index in that node's whole list.
    struct Line {
        int64_t node = 0;
        int32_t index = 0;
        bool operator==(const Line&) const = default;
    };

    /// The quest panel queued on top of the conversation (SubDialogueInstance, QuestAccept).
    struct Sub {
        int64_t              quest = 0;           // the quest on offer
        int64_t              node = 0;            // the node that offers it
        int64_t              previous_node = 0;   // the node the player left to get there
        int64_t              pending_next = 0;    // where the talk goes on after the quest is taken (0: it ends)
        std::vector<int64_t> objectives;          // the objectives the node names
        bool operator==(const Sub&) const = default;
    };

    /// CurrentDialogueInstance as the talking window uses it.
    struct State {
        uint64_t          source = 0;                 // the NPC (its actor id); 0: no conversation
        int64_t           previous_node = 0;
        int64_t           previous_choice_node = 0;
        int32_t           previous_choice_index = 0;
        int64_t           current_node = 0;           // 0 in the greeting
        int64_t           shown_node = 0;             // the node whose words are shown
        bool              greeting = false;
        std::vector<Line> lines;                      // the lines shown
        std::optional<Sub> sub;                       // the quest offer on top, if any
        bool operator==(const State&) const = default;
    };

    bool         open() const { return state_.source != 0; }
    const State& state() const { return state_; }

    /// The click on an NPC whose root is `root`: opens the conversation with its greeting, at `offered_node` (the node an offered quest names) when that is not 0, else at the root's
    /// top node.  Refused (nothing changes) when one is open, there is no conversation, it leads nowhere (an automatic node with no line that holds), starts at a node this
    /// milestone cannot show or at one with nothing to say.
    Result begin(const Book& book, Evaluator& ev, uint64_t source, int64_t root, int64_t offered_node = 0) {
        Result r;
        r.outcome = Outcome::Refused;
        if (open()) { r.why = "a conversation is open already"; return r; }
        const int64_t top = offered_node != 0 ? offered_node : book.top_node(root);
        if (top == 0) { r.why = book.has_root(root) ? "the root has no conversation" : "the root is not in the data"; return r; }
        std::vector<Pass> passes;
        std::string why;
        const auto shown = resolve(book, ev, top, passes, why);
        if (!shown) { r.why = why; return r; }
        const content::DialogueNodeRow* n = book.node(*shown);
        State s;
        s.source = source;
        s.shown_node = *shown;
        s.greeting = true;
        s.lines = visible(book, ev, *shown);
        if (!enter(book, *n, *shown, 0, s, r)) return r;
        noting(*n, r);
        state_ = std::move(s);
        r.outcome = Outcome::Changed;
        return r;
    }

    /// The player chose a line (ResponseType Choice).  Refused, changing nothing, for a conversation that is not open, a node other than the one shown or a line that
    /// is not shown.  Otherwise the line's actions are skipped (and listed); it leads to the next node, ends the conversation (no actions) or changes nothing.
    Result choose(const Book& book, Evaluator& ev, const protocol::repl::DialogueResponse& response) {
        Result r;
        r.outcome = Outcome::Refused;
        if (!open()) { r.why = "no conversation is open"; return r; }
        if (response.response_type != static_cast<uint8_t>(protocol::repl::DialogueResponseType::Choice)) { r.why = "not a choice"; return r; }
        if (state_.sub) { r.why = "a quest offer is open: the player answers it first"; return r; }
        if (response.node != state_.shown_node) {
            r.why = "the choice names node " + std::to_string(response.node) + ", the window shows " + std::to_string(state_.shown_node);
            return r;
        }
        bool shown = false;
        for (const Line& l : state_.lines) shown = shown || l.index == response.choice_index;
        if (!shown) { r.why = "line " + std::to_string(response.choice_index) + " is not among the lines shown"; return r; }
        const content::DialogueNodeRow* from = book.node(state_.shown_node);
        if (!from || response.choice_index < 0 || static_cast<size_t>(response.choice_index) >= from->choices.size()) { r.why = "the node has no such line"; return r; }
        const content::DialogueChoiceRow& line = from->choices[static_cast<size_t>(response.choice_index)];
        size_t others = 0;   // the actions that are not a shop's window
        for (const auto& a : line.actions) {
            if (a.action == "OpenWindow" && a.window == "Vendor" && a.vendor != 0) {
                if (r.open_vendor == 0) r.open_vendor = a.vendor;
                continue;
            }
            if (a.action == "ActionResponse" && a.response == "OpenPlayerWarehouse") {
                if (r.open_warehouse == 0) r.open_warehouse = a.access_config > 0 && a.access_config < 64 ? static_cast<int>(a.access_config) : 7;
                continue;
            }
            ++others;
            r.skipped.push_back(a.window.empty() ? a.action : a.action + "/" + a.window);
        }
        if (line.next == 0) {
            // a line that ends the conversation and opens a shop leaves the conversation as it is: the client closes its own window as the shop's opens
            if (r.open_vendor != 0) { r.outcome = Outcome::Nothing; r.why = "the line opens a shop"; return r; }
            // a line that opens the warehouse ends the conversation (the talking window clears) and the window opens in its place
            if (!line.actions.empty() && others > 0) { r.outcome = Outcome::Nothing; r.why = "the line's actions are not done yet"; return r; }
            state_ = State{};
            r.outcome = Outcome::Changed;
            return r;
        }
        std::vector<Pass> passes;
        std::string why;
        const auto target = resolve(book, ev, line.next, passes, why);
        if (!target) { r.outcome = Outcome::Nothing; r.why = why; return r; }
        const content::DialogueNodeRow* n = book.node(*target);
        State s = state_;
        s.previous_node = state_.current_node;
        s.previous_choice_node = from->guid;
        s.previous_choice_index = response.choice_index;
        s.current_node = line.next;
        for (const Pass& p : passes) {   // an automatic node passed through is the node the player came from, its chosen line the line chosen last
            const content::DialogueNodeRow* a = book.node(p.node);
            s.previous_node = p.node;
            s.previous_choice_node = p.node;
            s.previous_choice_index = p.index;
            s.current_node = a->choices[static_cast<size_t>(p.index)].next;
        }
        s.shown_node = *target;
        s.greeting = false;
        s.lines = visible(book, ev, *target);
        s.sub.reset();
        Result entered;
        if (n->type == "Response" && s.lines.empty()) {   // nothing to say: the talk ends here
            state_ = State{};
            r.outcome = Outcome::Changed;
            r.why = "the node " + std::to_string(*target) + " has no line that shows";
            return r;
        }
        if (!enter(book, *n, *target, state_.shown_node, s, entered)) { r.outcome = Outcome::Nothing; r.why = entered.why; return r; }
        r.offer = entered.offer;
        noting(*n, r);
        state_ = std::move(s);
        r.outcome = Outcome::Changed;
        return r;
    }

    /// The quest offer on top, if any.
    const std::optional<Sub>& sub() const { return state_.sub; }

    /// The player answered the quest panel and the caller has done what the answer asks (taken the quest, or not).  The queue is cleared; a quest that was taken goes on to the node the
    /// offer names when there is one that shows, else the talk ends, as it does for a quest that was declined or refused.  Nothing when no offer is open.
    Result finish_sub(const Book& book, Evaluator& ev, bool taken) {
        Result r;
        if (!open() || !state_.sub) return r;
        const Sub sub = *state_.sub;
        r.outcome = Outcome::Changed;
        r.offer_answered = true;
        if (taken && sub.pending_next != 0) {
            std::vector<Pass> passes;
            std::string why;
            if (const auto next = resolve(book, ev, sub.pending_next, passes, why)) {
                const content::DialogueNodeRow* n = book.node(*next);
                State s = state_;
                s.sub.reset();
                s.previous_node = sub.node;
                s.previous_choice_node = sub.node;
                s.previous_choice_index = 0;
                s.current_node = sub.pending_next;
                for (const Pass& p : passes) {
                    s.previous_node = p.node;
                    s.previous_choice_node = p.node;
                    s.previous_choice_index = p.index;
                    s.current_node = book.node(p.node)->choices[static_cast<size_t>(p.index)].next;
                }
                s.shown_node = *next;
                s.greeting = false;
                s.lines = visible(book, ev, *next);
                Result entered;
                if (n->type == "Response" && !s.lines.empty() && enter(book, *n, *next, sub.node, s, entered) && !entered.offer) {
                    noting(*n, r);
                    state_ = std::move(s);
                    return r;
                }
            }
        }
        state_ = State{};
        return r;
    }

    /// The window closed (the client's Close, a walk away, death, the NPC gone): every field back to nothing.  Nothing when it was not open.
    Result close() {
        Result r;
        if (!open()) return r;
        r.offer_answered = state_.sub.has_value();
        state_ = State{};
        r.outcome = Outcome::Changed;
        return r;
    }

private:
    struct Pass {
        int64_t node = 0;    // an automatic node passed through
        int32_t index = 0;   // the line it took
    };

    /// Whether the node can be shown, and what it adds to the state: an AcceptQuest node queues the quest panel, a Response node needs a line that shows (a greeting only: a step has
    /// its own rule above).  `from` is the node the player left.
    static bool enter(const Book& book, const content::DialogueNodeRow& n, int64_t guid, int64_t from, State& s, Result& r) {
        if (n.type == "AcceptQuest") {
            const content::QuestAcceptRow* offer = book.accept(guid);
            if (!offer || offer->quest == 0) { r.outcome = Outcome::Refused; r.why = "the offer node " + std::to_string(guid) + " is not in the data"; return false; }
            Sub sub;
            sub.quest = offer->quest;
            sub.node = guid;
            sub.previous_node = from;
            sub.pending_next = offer->on_success;
            sub.objectives = offer->objectives;
            s.sub = std::move(sub);
            r.offer = true;
            return true;
        }
        if (n.type != "Response") { r.outcome = Outcome::Refused; r.why = "the node is a " + n.type + " node, which this milestone cannot show"; return false; }
        if (s.lines.empty()) { r.outcome = Outcome::Refused; r.why = "the node " + std::to_string(guid) + " has no line that shows"; return false; }
        return true;
    }

    /// Follows automatic nodes from `guid` (each takes its first line whose condition holds) to a node with words.
    static std::optional<int64_t> resolve(const Book& book, Evaluator& ev, int64_t guid, std::vector<Pass>& passes, std::string& why) {
        for (int hops = 0;; ++hops) {
            const content::DialogueNodeRow* n = book.node(guid);
            if (!n) { why = "node " + std::to_string(guid) + " is not in the data"; return std::nullopt; }
            if (n->type != "Automatic") return guid;
            if (hops >= kMaxAutomaticSteps) { why = "more than " + std::to_string(kMaxAutomaticSteps) + " automatic nodes in a row"; return std::nullopt; }
            int taken = -1;
            for (size_t i = 0; i < n->choices.size() && taken < 0; ++i) {
                if (ev.holds(n->choices[i].requirement)) taken = static_cast<int>(i);
            }
            if (taken < 0) { why = "automatic node " + std::to_string(guid) + " has no line that holds"; return std::nullopt; }
            passes.push_back({guid, taken});
            guid = n->choices[static_cast<size_t>(taken)].next;
            if (guid == 0) { why = "an automatic line ends the conversation"; return std::nullopt; }
        }
    }

    /// The lines of a node whose conditions hold.
    static std::vector<Line> visible(const Book& book, Evaluator& ev, int64_t guid) {
        std::vector<Line> out;
        const content::DialogueNodeRow* n = book.node(guid);
        if (!n) return out;
        for (size_t i = 0; i < n->choices.size(); ++i) {
            if (ev.holds(n->choices[i].requirement)) out.push_back({guid, static_cast<int32_t>(i)});
        }
        return out;
    }

    /// What entering a node would run (its OnEnter actions) is skipped and listed.
    static void noting(const content::DialogueNodeRow& n, Result& r) {
        for (const auto& a : n.on_enter) r.skipped.push_back("OnEnter:" + (a.window.empty() ? a.action : a.action + "/" + a.window));
    }

    State state_;
};

}  // namespace aoc::worldd::dialogue

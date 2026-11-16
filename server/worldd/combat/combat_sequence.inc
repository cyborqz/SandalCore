// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_sequence.inc: the move a creature's tree plays next after the one that just ended (included by combat_world.cpp, inside its namespace)
//
//  The table carries the sequence as a link (CreatureAbility::next): a task's `next` holds every later task of the same tree that waits on a cooldown the task starts (the tree's own
//  readying, not a gate a large part of the tree shares).  When the cast is over the first of those tasks whose own conditions hold is played.  A chain step is let through the cooldown
//  that links the pair only: the timer named for the move that just ended or for the next one.  Every other cooldown, a gate the pair shares included, is asked as usual, so a chain never
//  skips it.  Every other condition is asked at the place and facing the creature has when its first move ends.  A chain step the tables hold back (a task `unknown` or `off`) is not
//  played and the chain is dropped: the creature goes on with its tree, so nothing waits on a move that cannot play.
// ============================================================================

namespace {

/// Whether a finished task starts a cooldown: its own timer, or a timer it starts with the task.
bool starts_a_cooldown(const CreatureAbility& t, const std::string& name) {
    if (!t.task.empty() && t.cooldown > 0 && t.task == name) return true;
    for (const CooldownStart& c : t.starts) {
        if (c.name == name) return true;
    }
    return false;
}

/// Whether a task waits on a cooldown: by its own timer, a gate, or a condition that asks for it (`cd(x)`).
bool waits_on_a_cooldown(const CreatureAbility& t, const std::string& name) {
    if (!t.task.empty() && t.task == name) return true;
    for (const std::string& gate : t.gates) {
        if (gate == name) return true;
    }
    for (const CondGroup& group : t.groups) {
        for (const CondToken& token : group) {
            if (token.atom.kind == CondAtom::Kind::Cooldown && token.atom.name == name) return true;
        }
    }
    return false;
}

/// Whether a cooldown is the one that links a finished task to its chain step: the finished task starts it and the step waits on it as a condition of its own.  The reader's `next`
/// holds only the pairs whose cooldown few tasks of the tree start and few wait on (a large part of the tree sharing one is a gate, not a readying), so a step is let through a condition
/// it asks even when the cooldown is named for neither move (the goblins' Rally starts `Pummel` and the Headbutts' condition asks it).  A cooldown named for one of the two moves is the
/// pair's own timer, as before; every other gate the pair shares is asked as usual.
bool links_the_pair(const CreatureAbility& from, const CreatureAbility& step, const std::string& name) {
    if (name.empty() || !starts_a_cooldown(from, name)) return false;
    for (const CondGroup& group : step.groups) {
        for (const CondToken& token : group) {
            if (token.atom.kind == CondAtom::Kind::Cooldown && token.atom.name == name) return true;
        }
    }
    return (name == from.task || name == step.task) && waits_on_a_cooldown(step, name);
}

}  // namespace

CombatWorld::Attack CombatWorld::choose_chain(Npc& n, CondContext ctx, double distance, double angle) {
    if (!n.anim || n.chain.empty()) return Attack{};
    std::vector<int64_t> chain;
    chain.swap(n.chain);
    const CreatureAbility* from = n.chain_from;
    n.chain_from = nullptr;
    for (const int64_t id : chain) {
        for (const CreatureAbility& t : *n.anim) {
            if (t.ability != id || t.unknown || t.off || !t.fight) continue;
            if (task_held(n, t)) continue;
            const abilities::AbilityDef* d = catalog_.ability(t.ability);
            if (!d || !animates(n, *d)) continue;
            const bool attack = is_attack(*d, catalog_, data_.ids().health);
            if (!attack && !is_alert_move(*d, catalog_, data_.ids().health) && !is_state_move(*d, catalog_, data_.ids().health)) continue;
            if (!attack && readies_an_attack(t, *n.anim, catalog_, data_.ids().health)) continue;
            ctx.cooldown_over = [this, &n, from, &t](const std::string& name) {
                if (from && links_the_pair(*from, t, name)) return true;
                const auto until = n.cooldown_until.find(name);
                return until == n.cooldown_until.end() || now_ >= until->second;
            };
            bool ready = true;
            for (const std::string& gate : t.gates) {
                if (from && links_the_pair(*from, t, gate)) continue;
                const auto until = n.cooldown_until.find(gate);
                if (until != n.cooldown_until.end() && now_ < until->second) {
                    ready = false;
                    break;
                }
            }
            if (!ready) continue;
            if (t.max_distance >= 0 && distance > t.max_distance) continue;
            if (t.min_distance >= 0 && distance < t.min_distance) continue;
            if (t.max_angle >= 0 && angle > t.max_angle) continue;
            bool holds = true;
            for (const CondGroup& group : t.groups) {
                if (!eval_group(group, ctx)) {
                    holds = false;
                    break;
                }
            }
            if (!holds) continue;
            Attack a;
            a.ability = d;
            a.task = &t;
            if (t.max_distance >= 0) a.range_cm = static_cast<int>(t.max_distance);
            return a;
        }
    }
    return Attack{};
}

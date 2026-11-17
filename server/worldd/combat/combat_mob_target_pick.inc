// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  combat/combat_mob_target_pick.inc
//
//  A creature's tree does not always fight the one the hate list names: some states first score the actors they know and take the best.  Such a state has a
//  utility-score selector condition: a candidate set (the characters it hates, the creatures on its side, or every actor it perceives), a filter each candidate
//  must pass (a distance band, line of sight, an effect, its health, the side it is on), a list of scores (distance, health, the facing, "behind", an effect,
//  a random term, the hate list's own target), a scheme that combines the scores (the product, the highest, the average) and the blackboard variable the winner
//  goes into.  This reads the selector the table folds to a `pick` condition: it scores each candidate, clamps each score to [0, 1], combines them, keeps the
//  best in the creature's blackboard, and returns it, so that the ability the state gates is aimed at it (or only gated by it, when the tree's task reads the
//  hate list's target instead).  A creature's spawned NPCs are not tracked yet: that candidate set picks nobody.
//  (included by combat_world.cpp)
// ============================================================================
#pragma once

namespace {
/// How far from the creature a selector looks for the actors it can name when the tree itself asks no distance (the game's perception is the notice range; this is the world's).
constexpr double kPickRangeCm = 3000.0;
}  // namespace

double CombatWorld::pick_score(const PickTerm& term, const Npc& caster, const PickCandidate& candidate, double now) {
    using K = PickTerm::Kind;
    const auto arg = [&](size_t i) { return i < term.args.size() ? pick_score(term.args[i], caster, candidate, now) : 0.0; };
    switch (term.kind) {
        case K::Number: return term.value;
        case K::Random: return probing_ ? 0.5 : rng_.rand_double();
        case K::RandomRange: return arg(0) + (arg(1) - arg(0)) * (probing_ ? 0.5 : rng_.rand_double());
        case K::Distance: return candidate.distance;
        case K::Health: return candidate.health;
        case K::HasEffect: return holds_effect(candidate.actor, term.effect, now) ? 1.0 : 0.0;
        case K::SourceHasEffect: return caster.holds_effect(term.effect, now) ? 1.0 : 0.0;
        case K::IsSelf: return candidate.actor == caster.actor_id ? 1.0 : 0.0;
        case K::IsMostHated: return candidate.actor != 0 && candidate.actor == caster.ai.target() ? 1.0 : 0.0;
        case K::IsFriendly: {
            if (candidate.actor == caster.actor_id) return 1.0;
            Fighter* f = fighter_of(candidate.actor);
            return f && data_.relation(caster.aligns, f->alignments()) != Relation::Hostile ? 1.0 : 0.0;
        }
        case K::Sees: return candidate.sees ? 1.0 : 0.0;
        case K::FacesAway: return candidate.facing;
        case K::SourceAngle: return candidate.angle;
        case K::CandidateFacing: return candidate.candidate_angle;
        case K::IsDead: {
            Fighter* f = fighter_of(candidate.actor);
            return f && !f->alive() ? 1.0 : 0.0;
        }
        case K::EliteRank: return candidate.rank;
        case K::SourceEliteRank: return caster.cs ? caster.cs->npc().elite_rank : 0.0;
        case K::Nearby: {
            const double within = arg(0);
            for (PlayerPort* p : players_) {
                if (p->actor_id() == caster.actor_id || !p->fighter().alive()) continue;
                if (distance_cm(caster.position(), p->fighter().position()) <= within) return 1.0;
            }
            std::vector<uint64_t> ids;
            const Vec3 at = caster.position();
            registry_.query(at.x, at.y, within, ids);
            for (const uint64_t id : ids) {
                if (id == caster.actor_id) continue;
                const world::WorldActor* a = registry_.find(id);
                if (a && a->kind == world::ActorKind::Npc && distance_cm(at, Vec3{a->x, a->y, a->z}) <= within) return 1.0;
            }
            return 0.0;
        }
        case K::Blackboard: {
            const auto it = caster.bb_targets.find(term.name);
            return it != caster.bb_targets.end() && it->second != 0 && it->second == candidate.actor ? 1.0 : 0.0;
        }
        case K::Alignment: {
            Fighter* f = fighter_of(candidate.actor);
            if (!f) return 0.0;
            for (const int64_t a : f->alignments()) if (a == term.effect) return 1.0;
            return 0.0;
        }
        case K::Add: return arg(0) + arg(1);
        case K::Sub: return arg(0) - arg(1);
        case K::Mul: return arg(0) * arg(1);
        case K::Div: return arg(1) == 0 ? 0.0 : arg(0) / arg(1);
        case K::Negate: return -arg(0);
        case K::Abs: return std::fabs(arg(0));
        case K::Max: return std::max(arg(0), arg(1));
        case K::Min: return std::min(arg(0), arg(1));
        case K::Clamp: return std::clamp(arg(0), arg(1), arg(2));
        case K::Lerp: return arg(0) + (arg(1) - arg(0)) * arg(2);
        case K::Choose: return arg(0) != 0 ? arg(1) : arg(2);
        case K::Lt: return arg(0) < arg(1) ? 1.0 : 0.0;
        case K::Le: return arg(0) <= arg(1) ? 1.0 : 0.0;
        case K::Gt: return arg(0) > arg(1) ? 1.0 : 0.0;
        case K::Ge: return arg(0) >= arg(1) ? 1.0 : 0.0;
        case K::Eq: return std::fabs(arg(0) - arg(1)) < 1e-9 ? 1.0 : 0.0;
        case K::Ne: return std::fabs(arg(0) - arg(1)) >= 1e-9 ? 1.0 : 0.0;
        case K::And: return arg(0) != 0 && arg(1) != 0 ? 1.0 : 0.0;
        case K::Or: return arg(0) != 0 || arg(1) != 0 ? 1.0 : 0.0;
        case K::Not: return arg(0) == 0 ? 1.0 : 0.0;
    }
    return 0.0;
}

std::vector<CombatWorld::PickCandidate> CombatWorld::pick_candidates(const Npc& caster, const PickRule& rule, double now) {
    std::vector<PickCandidate> out;
    const Vec3 at = caster.position();
    const world::WorldActor* me = registry_.find(caster.actor_id);
    const double yaw = me ? me->yaw_degrees * 3.14159265358979323846 / 180.0 : 0.0;
    const auto add = [&](uint64_t id) {
        if (id == 0 || std::any_of(out.begin(), out.end(), [id](const PickCandidate& c) { return c.actor == id; })) return;
        Fighter* f = fighter_of(id);
        if (!f || !f->alive()) return;
        PickCandidate c;
        c.actor = id;
        c.player = f->is_player();
        const Vec3 p = f->position();
        c.distance = distance_cm(at, p);
        const double dx = p.x - at.x, dy = p.y - at.y;
        c.facing = c.distance > 0 ? (std::cos(yaw) * dx + std::sin(yaw) * dy) / c.distance : 1.0;
        c.angle = std::acos(std::clamp(c.facing, -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
        const double towards = std::atan2(at.y - p.y, at.x - p.x) * 180.0 / 3.14159265358979323846;
        if (c.player) {
            c.candidate_angle = angle_between_degrees(towards, f->facing_degrees());
        } else if (const world::WorldActor* a = registry_.find(id)) {
            c.candidate_angle = angle_between_degrees(towards, a->yaw_degrees);
        }
        const stats::StatValue* h = f->stats().find(content::kGuidHealth);
        const stats::StatValue* m = f->stats().find(content::kGuidMaxHealth);
        c.health = h && m && m->value > 0 ? std::clamp(static_cast<double>(h->value) / m->value, 0.0, 1.0) : 1.0;
        c.rank = f->stats().npc().elite_rank;
        c.sees = line_clear(at, p);
        out.push_back(std::move(c));
    };
    // the actors near the creature that a selector can name: the ones it perceives (no perception list is kept: every living actor inside its notice range)
    const auto near = [&]() {
        std::vector<uint64_t> ids;
        registry_.query(at.x, at.y, kPickRangeCm, ids);
        for (const uint64_t id : ids) {
            const world::WorldActor* a = registry_.find(id);
            if (!a || a->kind != world::ActorKind::Npc) continue;
            Fighter* f = fighter_of(id);
            if (f && f->alive()) add(id);
        }
    };
    switch (rule.candidates) {
        case PickRule::Candidates::Hated: {
            // the characters the creature hates: the players its threat table holds, the one it fights first
            std::vector<std::pair<double, uint64_t>> hated;
            for (const npc::ai::ThreatTable::Entry& e : caster.ai.threat().entries()) {
                if (player(e.id)) hated.emplace_back(-e.threat, e.id);
            }
            std::sort(hated.begin(), hated.end());
            for (const auto& [_, id] : hated) add(id);
            break;
        }
        case PickRule::Candidates::Friendly: {
            add(caster.actor_id);
            std::vector<uint64_t> ids;
            registry_.query(at.x, at.y, kPickRangeCm, ids);
            for (const uint64_t id : ids) {
                if (id == caster.actor_id) continue;
                const world::WorldActor* a = registry_.find(id);
                if (!a || a->kind != world::ActorKind::Npc) continue;
                Fighter* f = fighter_of(id);
                if (f && f->alive() && data_.relation(caster.aligns, f->alignments()) != Relation::Hostile) add(id);
            }
            break;
        }
        case PickRule::Candidates::Perceived: {
            add(caster.actor_id);
            for (PlayerPort* p : players_) add(p->actor_id());
            near();
            break;
        }
        case PickRule::Candidates::Spawned:
            // (a creature's spawned NPCs are not tracked yet: no candidate names one)
            return {};
    }
    // the tree's filters: a candidate counts when every one of them holds
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const PickCandidate& c) {
                                 for (const PickTerm& f : rule.filters) {
                                     if (!(pick_score(f, caster, c, now) > 0.5)) return true;
                                 }
                                 return false;
                             }),
              out.end());
    if (rule.sort_by_health) {
        std::stable_sort(out.begin(), out.end(), [](const PickCandidate& a, const PickCandidate& b) {
            if (a.health != b.health) return a.health < b.health;
            return a.distance != b.distance ? a.distance < b.distance : a.actor < b.actor;
        });
    } else {
        std::stable_sort(out.begin(), out.end(), [](const PickCandidate& a, const PickCandidate& b) { return a.distance != b.distance ? a.distance < b.distance : a.actor < b.actor; });
    }
    if (rule.crop > 0 && out.size() > rule.crop) out.resize(rule.crop);
    return out;
}

double CombatWorld::pick_combined(const PickRule& rule, const Npc& caster, const PickCandidate& c, double now) {
    double score = rule.scheme == PickRule::Scheme::Product ? 1.0 : 0.0;
    double sum = 0;
    for (const PickTerm& term : rule.considerations) {
        const double value = std::clamp(pick_score(term, caster, c, now), 0.0, 1.0);
        switch (rule.scheme) {
            case PickRule::Scheme::Product: score *= value; break;
            case PickRule::Scheme::Highest: score = std::max(score, value); break;
            case PickRule::Scheme::Average: sum += value; break;
        }
    }
    if (rule.scheme == PickRule::Scheme::Average) score = rule.considerations.empty() ? 1.0 : std::clamp(sum / static_cast<double>(rule.considerations.size()), 0.0, 1.0);
    return score;
}

uint64_t CombatWorld::pick_target(Npc& caster, const PickRule& rule, double now) {
    const std::vector<PickCandidate> candidates = pick_candidates(caster, rule, now);
    uint64_t best = 0;
    double best_score = 0;
    for (const PickCandidate& c : candidates) {
        const double score = pick_combined(rule, caster, c, now);
        // the best wins, and a tie keeps the candidate the tree saw first (`Greater`, not `GreaterOrEqual`, in the selector's own comparison)
        if (score > best_score) {
            best_score = score;
            best = c.actor;
        }
    }
    if (best != 0) {
        if (!rule.bb.empty()) caster.bb_targets[rule.bb] = best;
        return best;
    }
    if (!rule.bb.empty() && rule.clear_on_fail) caster.bb_targets.erase(rule.bb);
    return 0;
}

double CombatWorld::pick_reach(Npc& caster, const PickRule& rule) {
    const auto known = pick_reaches_.find(&rule);
    if (known != pick_reaches_.end()) return known->second;
    // A candidate that is otherwise neutral (full health, straight ahead, in sight, carrying nothing) at each distance in turn: where the selector can still name it is how far the
    // creature's move reaches.  A selector that names it everywhere bounds nothing, and one that never names it (a move for a player behind it) has no reach to give.
    probing_ = true;
    double reach = 0;
    double last = 0;
    for (double d = 50.0; d <= kPickRangeCm; d += 50.0) {
        PickCandidate c;
        c.distance = d;
        bool passes = true;
        for (const PickTerm& f : rule.filters) {
            if (!(pick_score(f, caster, c, now_) > 0.5)) {
                passes = false;
                break;
            }
        }
        last = passes && pick_combined(rule, caster, c, now_) > 0 ? d : 0.0;
        if (last > 0) reach = last;
    }
    probing_ = false;
    if (last > 0) reach = 0;   // (it still names the candidate at the far end: the distance is no bound)
    pick_reaches_[&rule] = reach;
    return reach;
}

void CombatWorld::bind_pick(Attack& a, Npc& n, const CreatureAbility& t, const CondContext& ctx, bool attack) {
    uint64_t gate = 0;
    bool aimed = false;
    for (const CondGroup& g : t.groups) {
        for (const CondToken& token : g) {
            if (token.atom.kind != CondAtom::Kind::Pick) continue;
            // the reach a selector bounds is the creature's own reach for an attack that names none: the Pounce is cast from the 5 to 15 m its selector scores, not from where it bites
            if (attack && t.max_distance < 0) a.range_cm = std::max(a.range_cm, static_cast<int>(pick_reach(n, token.atom.pick)));
            const auto winner = ctx.picks.find(&token.atom.pick);
            if (winner == ctx.picks.end() || winner->second == 0 || aimed) continue;
            // the selector whose blackboard the task reads names the move's target; one that only gates the state names it only when the move is no attack
            if (token.atom.pick.casts_at_pick) {
                a.pick = winner->second;
                aimed = true;
            } else if (gate == 0) {
                gate = winner->second;
            }
        }
    }
    a.pick_casts = aimed;
    if (!aimed && gate != 0) {
        a.pick = gate;
        a.pick_casts = !attack;
    }
}

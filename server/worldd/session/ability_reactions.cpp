// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/ability_reactions.cpp  (see ability_pipeline.h)
//
//  The game's reactions (contentc version 35): a small rule an effect carries while it is on its holder. When an event of a kind it is bound to happens to
//  the holder (a hit the holder made lands, a hit he takes, an ability's cost is paid) and its trigger expression is true, the reaction applies its hits:
//  to the holder (Owner), to the targets of the hit that raised the event (Targets), or to the actor that struck him (HitSource, HitInstigator), under the
//  reaction's own source so that the client takes them as the server's.
//
//  How the game does it [the client's code, which shares the logic with the server]:
//    - the holder's effects that are registered for the kind of the event are walked in the order they were added, and each one's reactions in the order they
//      are listed: the binding is tested first (one byte of bits per event kind), then the trigger;
//    - a reaction's hits are applied in order through the ordinary hit path; a hit that a reaction made raises no reaction (the data repeats it in its triggers);
//    - a hit that fires further hits (RecursiveHit) fires them when it lands, aimed by the targeting record each names: Default_Self at the caster, a sphere
//      at the actors its filter and score pick around the actor the parent landed on; a child that fires a projectile sends its landing hit there after its flight;
//    - a hit raises its event only when its record has event tags or stat mods;
//    - a limit on how often a reaction may fire (GlobalProcLimit, InstigatorProcLimit) is counted per reaction here (one holder), and a carrier that is a
//      pipeline effect is taken off when its reaction reached it.
//
//  What this file serves: the events Hit (OwnerHits and OwnerIsHit) and Ability (Consume), the instigators Owner, Instigator, HitSource and HitInstigator
//  (the last two are the holder himself for a hit he made), and the targets Owner (the holder), Targets (the hit's), Instigator, HitSource and HitInstigator.
//  The rest of the bindings and instigators of the 335 records are left alone and said once in the log when a reaction would need them.
// ============================================================================
#include "session/ability_pipeline.h"

#include "content/content_db.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using abilities::ReactionDef;
using bootstrap::AbilityChange;

namespace {

// The EReactionTarget bits of a reaction's TargetsBitmask.
constexpr int kTargetOwner = 1;
constexpr int kTargetTargets = 8;
constexpr int kTargetInstigator = 2;
constexpr int kTargetHitSource = 4;
constexpr int kTargetHitInstigator = 16;
constexpr int kTargetInstigatorsPet = 8192;

// The event kinds (the byte of the binding the reaction listens with) and the bits this file serves.
constexpr int kKindHit = 0;
constexpr int kKindAbility = 2;
constexpr int kBitOwnerIsHit = 2;
constexpr int kBitOwnerHits = 4;
constexpr int kBitConsume = 4;

// A chain of further hits goes this deep: the melodies' targeting hit, its projectile's landing hit, and the spend that landing hit fires.
constexpr int kMaxChildDepth = 3;

int binding_of(const content::ReactionRow& r, int kind) {
    switch (kind) {
        case kKindHit: return r.bind_hit;
        case kKindAbility: return r.bind_ability;
        default: return 0;
    }
}

}  // namespace

void AbilityPipeline::set_reaction_carriers(std::vector<int64_t> effect_guids) {
    reaction_carriers_ = std::move(effect_guids);
}

void AbilityPipeline::fire_carried_reactions(AbilityChange& c, const HitEvent& ev, double t) {
    if (!reactions_enabled_ || carried_depth_ >= 2 || !catalog_ || !ev.hit || ev.hit->reactions.empty() || (ev.targets.empty() && !ev.on_self)) return;
    ReactionContext rc;
    rc.hit = &ev;
    for (const int64_t guid : ev.hit->reactions) {
        const ReactionDef* r = catalog_->reaction(guid);
        if (!r) continue;
        const content::ReactionRow& row = r->row;
        if (row.trigger_expr != 0) {
            double v = 0;
            std::string why;
            if (!eval_in_event(row.trigger_expr, rc, v, why)) {
                if (reaction_noted_.insert(guid).second) spdlog::info("[abilities] the reaction {} stays inert: {}", row.name, why);
                continue;
            }
            if (v == 0) continue;
        }
        HitSource src;
        src.type = content::kSourceTypeReaction;
        src.guid = guid;
        src.actor = false;
        src.confirm = true;
        ++carried_depth_;
        const ReactionContext* const outer = active_reaction_;
        active_reaction_ = &rc;
        for (const int64_t hit_guid : row.hits) {
            const HitDef* hit = catalog_->hit(hit_guid);
            if (!hit) continue;
            if ((row.targets_mask & (kTargetOwner | kTargetInstigator)) != 0) apply_hit_here(c, *hit, t, true, src);   // (the caster's: its own mask speaks of whoever casts it)
            if ((row.targets_mask & kTargetTargets) != 0 && !ev.targets.empty()) apply_hit_on_targets(c, *hit, ev.targets, t, src, nullptr);
            if ((row.targets_mask & kTargetInstigatorsPet) != 0 && combat_) {
                if (const uint64_t pet = combat_->own_pet()) apply_hit_on_targets(c, *hit, {pet}, t, src, nullptr);
            }
        }
        active_reaction_ = outer;
        drop_evaluation();
        --carried_depth_;
        ++reactions_fired_;
        spdlog::log(reactions_fired_ <= 40 ? spdlog::level::info : spdlog::level::debug, "[abilities] the reaction {} of the hit {} fired", row.name, ev.hit->row.name);
    }
}

void AbilityPipeline::raise_hit_event(AbilityChange& c, const HitEvent& ev, double t) {
    fire_carried_reactions(c, ev, t);
    if (!reactions_enabled_ || reaction_depth_ > 0 || !catalog_ || catalog_->reactions() == 0 || !ev.hit) return;
    // a hit with no event tags and no stat mods raises no event [the game's code: the hit's record is tested for tags, mods and the force flag]
    if (ev.hit->tags.empty() && ev.hit->mods.empty()) return;
    // a hit a reaction made raises no reaction
    if (ev.src.type == content::kSourceTypeReaction) return;
    ReactionContext rc;
    rc.hit = &ev;
    run_reactions(c, kKindHit, kBitOwnerHits, rc, t);
}

void AbilityPipeline::raise_hit_taken(AbilityChange& c, int64_t hit_guid, int64_t source_type, int64_t source_guid, uint64_t attacker, uint8_t flags,
                                      const std::vector<combat::ResolvedMod>& mods, const std::vector<double>& effective, double t, bool apply) {
    if (!reactions_enabled_ || reaction_depth_ > 0 || !catalog_ || catalog_->reactions() == 0) return;
    // a hit a reaction made raises no reaction [the game's code; the creature path holds it the same way]
    if (source_type == content::kSourceTypeReaction) return;
    const HitDef* hit = catalog_->hit(hit_guid);
    if (!hit) return;
    HitEvent event;
    event.hit = hit;
    event.src.type = source_type;
    event.src.guid = source_guid;
    event.src.actor = false;
    event.attacker = attacker;
    event.on_self = true;
    event.flags = flags;
    for (size_t i = 0; i < mods.size(); ++i) {
        if (!std::isfinite(mods[i].value)) continue;
        // the event's stat mod is the part the hit took effect with (a miss, a block or a shield leaves nothing): the data's triggers read it to answer
        // "did this hit take Health?" (Thorns, Reflect and the like), and a hit that took none raises no reaction
        const double took = i < effective.size() ? effective[i] : (apply ? 0.0 : mods[i].value);
        event.mods[mods[i].stat_guid] += took;
        event.effective[mods[i].stat_guid] += took;
    }
    // a being-hit event reads the same things an own hit's does: the hit's record, its tags and its stat mods; the attacker is HitSource and HitInstigator
    ReactionContext rc;
    rc.hit = &event;
    rc.taking = true;
    run_reactions(c, kKindHit, kBitOwnerIsHit, rc, t);
}

void AbilityPipeline::raise_hit_taken(const combat::IncomingHit& in, double now) {
    advance(now);
    AbilityChange c;
    // the numbers went to the mount: for the rider's reactions the hit took effect in full (a miss carries no mods)
    std::vector<double> effective;
    effective.reserve(in.mods.size());
    for (const combat::ResolvedMod& m : in.mods) effective.push_back(std::isfinite(m.value) ? m.value : 0.0);
    raise_hit_taken(c, in.hit_guid, in.source_type, in.source_guid, in.instigator, in.flags, in.mods, effective, now_, false);
    recompute(c);
    emit({std::move(c), std::nullopt, now_, "a hit on the character's mount"});
}

void AbilityPipeline::raise_ability_event(AbilityChange& c, const AbilityDef& ability, double t) {
    if (!reactions_enabled_ || reaction_depth_ > 0 || !catalog_ || catalog_->reactions() == 0) return;
    ReactionContext rc;
    rc.consumed = &ability;
    run_reactions(c, kKindAbility, kBitConsume, rc, t);
}

void AbilityPipeline::run_reactions(AbilityChange& c, int kind, int bit, const ReactionContext& rc, double t) {
    // the carriers in the order the character got them: the ranks' passives first, then what abilities and hits applied (a snapshot: a reaction's hits change
    // the effects)
    std::vector<int64_t> carriers = reaction_carriers_;
    for (const EffectInst& e : effects_) carriers.push_back(e.def->row.guid);
    std::vector<int64_t> seen;
    for (const int64_t effect_guid : carriers) {
        if (std::find(seen.begin(), seen.end(), effect_guid) != seen.end()) continue;
        seen.push_back(effect_guid);
        const EffectDef* carrier = catalog_->effect(effect_guid);
        if (!carrier || carrier->reactions.empty()) continue;
        for (const int64_t reaction_guid : carrier->reactions) {
            const ReactionDef* r = catalog_->reaction(reaction_guid);
            if (!r || (binding_of(r->row, kind) & bit) == 0) continue;
            const int limit = r->row.global_limit > 0 ? r->row.global_limit : r->row.instigator_limit;
            if (limit > 0 && reaction_procs_[reaction_guid] >= limit) continue;
            if (r->row.trigger_expr != 0) {
                double v = 0;
                std::string why;
                if (!eval_in_event(r->row.trigger_expr, rc, v, why)) {
                    if (reaction_noted_.insert(reaction_guid).second) spdlog::info("[abilities] the reaction {} stays inert: {}", r->row.name, why);
                    continue;
                }
                if (v == 0) continue;
            }
            fire_reaction(c, *r, rc, t);
            if (limit > 0 && reaction_procs_[reaction_guid] >= limit) {
                // a reaction that reached its limit takes its carrier off [the game's code: the effect goes with all its stacks]
                for (const EffectInst& e : effects_) {
                    if (e.def->row.guid == effect_guid) {
                        remove_stacks(c, effect_guid, 1 << 20);
                        break;
                    }
                }
            }
        }
    }
}

void AbilityPipeline::fire_reaction(AbilityChange& c, const ReactionDef& r, const ReactionContext& rc, double t) {
    const content::ReactionRow& row = r.row;
    const uint64_t self = combat_ ? combat_->self().id() : 0;
    // who casts the hits: the holder (Owner), the instigator of the effect that carries the reaction (Instigator), or, for a hit he made, the hit's
    // instigator or source (the holder again).  Every effect this pipeline holds is the character's own, so its instigator is the holder himself (Shatter
    // and Fuse, carried by the Mage's passives).  A being-hit reaction cast by the attacker needs the attacker's stats, which this pipeline does not have.
    uint64_t caster = self;
    if (row.instigator == "Owner" || row.instigator == "Instigator") {
        caster = self;
    } else if (row.instigator == "HitInstigator" || row.instigator == "HitSource") {
        caster = rc.taking && rc.hit ? rc.hit->attacker : self;
    } else {
        if (reaction_noted_.insert(row.guid).second) spdlog::info("[abilities] the reaction {} is cast by {}, which is not served yet: it does not fire", row.name, row.instigator);
        return;
    }
    if (caster != self || (caster == 0 && rc.taking)) {
        if (reaction_noted_.insert(row.guid).second) {
            spdlog::info("[abilities] the reaction {} is cast by actor {}, whose stats this character's runtime does not hold: it does not fire", row.name, caster);
        }
        return;
    }
    const int served = kTargetOwner | kTargetInstigator | kTargetTargets | kTargetHitSource | kTargetHitInstigator;
    if ((row.targets_mask & ~served) != 0 && reaction_noted_.insert(-row.guid).second) {
        spdlog::info("[abilities] the reaction {} lands on targets (mask {}) that are not all served yet: the holder, the hit's targets and its actor get its hits", row.name, row.targets_mask);
    }
    if ((row.targets_mask & served) == 0) return;
    ++reaction_procs_[row.guid];
    ++reactions_fired_;
    // the reaction's hits are the game's own records, applied under the reaction as their source: the client takes them as the server's (predicted on neither
    // side) and draws their numbers from the confirmations
    HitSource src;
    src.type = content::kSourceTypeReaction;
    src.guid = row.guid;
    src.actor = false;
    src.confirm = true;
    src.autonomous = false;
    src.simulated = false;
    // where its hits land: the holder (Owner, Instigator), the hit's targets, or the actor that struck him (HitSource, HitInstigator; for a hit he made,
    // his own)
    const bool on_owner = (row.targets_mask & (kTargetOwner | kTargetInstigator)) != 0;
    const bool on_targets = (row.targets_mask & kTargetTargets) != 0;
    const bool on_attacker = (row.targets_mask & (kTargetHitSource | kTargetHitInstigator)) != 0;
    // for a hit the holder made, the hit's source and instigator are the holder himself: those masks land on him
    const bool on_self = on_owner || (on_targets && rc.taking) || (on_attacker && !rc.taking);
    std::vector<uint64_t> others;
    if (!rc.taking) {
        if (on_targets && rc.hit) others = rc.hit->targets;
    } else if (on_attacker && rc.hit && rc.hit->attacker != 0 && rc.hit->attacker != self) {
        others.push_back(rc.hit->attacker);
    }
    ++reaction_depth_;
    const ReactionContext* const outer = active_reaction_;
    active_reaction_ = &rc;   // (a reaction's formulas read the event: the Tank's Reflect scales with the Health the hit took)
    size_t applied = 0;
    for (const int64_t hit_guid : row.hits) {
        const HitDef* hit = catalog_->hit(hit_guid);
        if (!hit) continue;
        if (on_self) {
            const bool held_by_him = hits_caster(hit->row);
            apply_hit(c, *hit, t, true, src);
            ++applied;
            // the hit it fires further (RecursiveHit) follows it only when it actually landed on him
            if (held_by_him) follow_hit_children(c, *hit, self, t, src, 0);
        }
        if (!others.empty()) {
            bool landed = false;
            bool took_effect = false;
            uint64_t first = 0, first_reached = 0;
            apply_hit_on_targets(c, *hit, others, t, src, nullptr, &landed, &first, &first_reached, &took_effect);
            if (took_effect) ++applied;
            if (landed) follow_hit_children(c, *hit, first_reached != 0 ? first_reached : first, t, src, 0);
        }
    }
    active_reaction_ = outer;
    --reaction_depth_;
    spdlog::log(reactions_fired_ <= 40 ? spdlog::level::info : spdlog::level::debug, "[abilities] the reaction {} fired ({} of its {} hit(s) applied; event: {})", row.name, applied, row.hits.size(),
                rc.hit && rc.hit->hit ? "a hit, " + rc.hit->hit->row.name : rc.consumed ? "the cost of " + rc.consumed->row.name + " paid" : std::string("?"));
}

// ---- the hits a hit fires when it lands (RecursiveHit) ------------------------------------------------------------------------------------

namespace {

/// The actors of one side a targeting record's sphere reaches, before its filter: the child's mask says which side (an enemy, or an ally, the caster
/// himself too).
struct TargetCand {
    uint64_t actor = 0;
    double   score = 0, distance = 0;
    int      entry = 0;   // the sorting entry that let the actor through: the actors of an earlier entry come before those of a later one
};

}  // namespace

bool AbilityPipeline::fires_party_aura(const HitDef& hit) const {
    if (!combat_ || !catalog_) return false;
    for (const content::RecursiveHitRow& r : hit.recursive) {
        const HitDef* child = r.targeting != 0 ? catalog_->hit(r.child_hit) : nullptr;
        if (!child) continue;
        const int mask = child->row.applicable_targets;
        if ((mask & (8 | 16)) == 0 || (mask & 4) != 0) continue;   // the party, and not every ally
        const content::TargetingDefRow* def = catalog_->targeting_def(r.targeting);
        if (def && !def->shape.empty() && def->radius > 0) return true;
    }
    return false;
}

std::vector<uint64_t> AbilityPipeline::targeting_targets(const HitDef& child, const content::TargetingDefRow& def, const combat::Vec3& centre) {
    std::vector<TargetCand> candidates;
    if (!combat_ || !(def.radius > 0)) return {};
    const uint64_t self = combat_->self().id();
    const int mask = child.row.applicable_targets;
    // a mask that names the party (or the raid) and no Ally reaches the caster and the members of his party only (a melody's Buff); with Ally it reaches every friendly player
    const bool party_only = (mask & (8 | 16)) != 0 && (mask & 4) == 0;
    if ((mask & 2) != 0) {
        for (const combat::GatheredTarget& g : combat_->enemies_around(centre, def.radius, 40)) {
            const combat::TargetInfo ti = combat_->inspect(g.actor);
            if (!ti.exists || !ti.alive) continue;
            if (def.los && !combat_->line_clear(centre, ti.position)) continue;
            double score = 0;
            int entry = 0;
            if (!targeting_score(def, g.actor, ti, score, entry)) continue;
            candidates.push_back({g.actor, score, ti.surface_distance_cm, entry});
        }
    }
    if ((mask & (1 | 4 | 8 | 16)) != 0) {
        bool self_seen = false;
        const std::vector<combat::AllyInfo> around = combat_->players_around(centre, def.radius);
        // the score of "favors yourself and party members" asks whether the actor is of the caster's party: the world's own party flag, read before any scoring
        std::set<uint64_t> party;
        for (const combat::AllyInfo& a : around) {
            if (a.party) party.insert(a.actor);
        }
        if (self != 0) party.insert(self);
        struct PartyScope {
            const std::set<uint64_t>*& slot;
            const std::set<uint64_t>*  outer;
            PartyScope(const std::set<uint64_t>*& s, const std::set<uint64_t>* now) : slot(s), outer(s) { slot = now; }
            ~PartyScope() { slot = outer; }
        } scope(party_ids_, &party);
        for (const combat::AllyInfo& a : around) {
            const combat::TargetInfo ti = combat_->inspect(a.actor);
            if (!ti.exists || !ti.alive) continue;
            if (party_only && !a.self && !a.party) continue;
            if (def.los && a.actor != self && !combat_->line_clear(centre, ti.position)) continue;
            self_seen = self_seen || a.actor == self;
            double score = 0;
            int entry = 0;
            if (!targeting_score(def, a.actor, ti, score, entry)) continue;
            candidates.push_back({a.actor, score, ti.surface_distance_cm, entry});
        }
        // a hit that lands on the caster alone (a mask of Self): his own players list holds him, but a world that does not know its own pawn does not
        if ((mask & 1) != 0 && !self_seen && self != 0) {
            combat::TargetInfo own;
            own.exists = true;
            own.alive = true;
            own.position = combat_->self().position();
            own.radius_cm = combat_->self().radius_cm();
            own.relation = combat::Relation::Friendly;
            double score = 0;
            int entry = 0;
            if (targeting_score(def, self, own, score, entry)) candidates.push_back({self, score, 0.0, entry});
        }
    }
    // the entries are tried in order and each one's actors are ranked by its own score (the injured allies of the first entry before every other ally of the
    // second, whatever the two scores' scales)
    std::sort(candidates.begin(), candidates.end(), [](const TargetCand& x, const TargetCand& y) {
        if (x.entry != y.entry) return x.entry < y.entry;
        if (x.score != y.score) return x.score > y.score;
        if (x.distance != y.distance) return x.distance < y.distance;
        return x.actor < y.actor;
    });
    double cap = 40;
    if (child.row.max_targets_expr != 0) {
        double v = 0;
        std::string why;
        if (eval(child.row.max_targets_expr, v, why) && v >= 1) cap = std::min(cap, v);
    }
    std::vector<uint64_t> out;
    for (const TargetCand& c : candidates) {
        if (static_cast<double>(out.size()) >= cap) break;
        out.push_back(c.actor);
    }
    return out;
}

bool AbilityPipeline::targeting_score(const content::TargetingDefRow& def, uint64_t actor, const combat::TargetInfo& ti, double& out, int& index) {
    index = 0;
    if (def.sorting.empty()) {
        out = -ti.surface_distance_cm;
        return true;
    }
    for (size_t at = 0; at < def.sorting.size(); ++at) {
        const content::TargetingSortRow& entry = def.sorting[at];
        index = static_cast<int>(at);
        // Score: the entry's own expression; Distance: the nearest first (the aura records, whose score is 0)
        if (!entry.method.empty() && entry.method != "Score" && entry.method != "Distance") continue;
        if (entry.filter != 0) {
            double v = 0;
            std::string why;
            if (!eval_target(entry.filter, actor, ti, v, why)) return false;   // (a filter that cannot be read lets no actor through)
            if (v == 0) continue;
        }
        double score = 0;
        if (entry.score != 0) {
            std::string why;
            if (!eval_target(entry.score, actor, ti, score, why)) return false;
        } else if (entry.method == "Distance") {
            score = -ti.surface_distance_cm;
        }
        out = score;
        return true;
    }
    return false;
}

void AbilityPipeline::follow_hit_children(AbilityChange& c, const HitDef& hit, uint64_t landed_on, double t, const HitSource& src, int depth) {
    if (!combat_ || !catalog_ || depth >= kMaxChildDepth) return;
    const uint64_t self = combat_->self().id();
    const combat::Vec3 centre = landed_on != 0 && landed_on != self ? combat_->inspect(landed_on).position : combat_->self().position();
    for (const content::RecursiveHitRow& r : hit.recursive) {
        const HitDef* child = catalog_->hit(r.child_hit);
        if (!child) continue;
        const content::TargetingDefRow* def = r.targeting != 0 ? catalog_->targeting_def(r.targeting) : nullptr;
        std::vector<uint64_t> ids;
        if (def && !def->shape.empty() && def->radius > 0) {
            ids = targeting_targets(*child, *def, centre);
        } else if (r.targeting == content::kTargetingDefaultSelf) {
            if ((child->row.applicable_targets & 1) != 0) ids.push_back(self);
        } else if (landed_on != 0) {
            // Default_Recursive (the source of the parent's hit) and a record that names one actor: the actor the parent landed on
            ids.push_back(landed_on);
        }
        if (ids.empty()) {
            if (reaction_recursive_noted_.insert(r.child_hit).second) {
                spdlog::info("[abilities] the hit {} fires {}, which the targeting record {} aims at no actor this runtime serves yet", hit.row.name, child->row.name,
                             def ? def->name : std::to_string(r.targeting));
            }
            continue;
        }
        HitSource child_src = src;
        child_src.confirm = true;
        child_src.reach_free = true;
        for (const uint64_t id : ids) {
            bool landed = false;
            if (id == self) {
                if (!hits_caster(child->row)) continue;
                apply_hit(c, *child, t, true, child_src);
                landed = true;
            } else {
                apply_hit_on_targets(c, *child, {id}, t, child_src, nullptr, &landed);
            }
            if (!landed) continue;
            // a child that fires a projectile sends its landing hit at the actor after the flight; that hit follows its own children (the melodies' spend)
            for (const int64_t guid : child->fires) {
                const content::ProjectileRow* p = catalog_->projectile(guid);
                if (!p || !(p->speed > 0)) continue;
                double distance = 0;
                if (id != self) {
                    const combat::TargetInfo ti = combat_->inspect(id);
                    const combat::Vec3 me = combat_->self().position();
                    distance = std::hypot(ti.position.x - me.x, ti.position.y - me.y);
                }
                for (const int64_t impact : p->hits) {
                    const HitDef* landing = catalog_->hit(impact);
                    if (!landing || delayed_.size() >= kMaxDelayed) continue;
                    DelayedChild d;
                    d.due = t + distance / p->speed;
                    d.hit = landing;
                    d.ability = nullptr;
                    d.src.type = content::kSourceTypeProjectile;
                    d.src.guid = guid;
                    d.src.actor = false;
                    d.src.confirm = true;
                    d.src.simulated = true;
                    d.src.autonomous = true;
                    d.src.reach_free = true;
                    d.targets = {id};
                    d.children = true;
                    d.depth = depth + 1;
                    delayed_.push_back(std::move(d));
                }
            }
            // the hits the child itself fires follow it for every actor it reached: a multi-target child's own child (Epic's Cost1 on the AoE hit) runs once
            // for each of them, as the runtime applies the child once per target
            follow_hit_children(c, *child, id, t, child_src, depth + 1);
        }
    }
}

}  // namespace aoc::worldd

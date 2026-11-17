// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_world_npc.h: one NPC's combat state, for the source files that make up CombatWorld (combat_world.cpp, combat_pets.cpp).  Nothing outside them includes it.
// ============================================================================
#pragma once

#include "combat/combat_world.h"
#include "combat/npc_ability_replicator.h"
#include "combat/npc_replicator.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::combat {

namespace npc_kits {
struct Kit;
}

// ---- one NPC's combat state ----------------------------------------------------------------------------------------------------------

class CombatWorld::Npc final : public Fighter {
public:
    Npc(CombatWorld& w, uint64_t actor, npc::MotionStats bp_in, std::string leaf_in, int level_in)
        : world(w), actor_id(actor), bp(std::move(bp_in)), leaf(std::move(leaf_in)), lvl(level_in) {}

    uint64_t id() const override { return actor_id; }
    bool     is_player() const override { return false; }
    int      level() const override { return lvl; }
    Vec3     position() const override {
        const world::WorldActor* a = world.registry_.find(actor_id);
        return a ? Vec3{a->x, a->y, a->z} : Vec3{};
    }
    double radius_cm() const override { return bp.radius; }
    bool   alive() const override { return !dead; }
    const std::vector<int64_t>& alignments() const override { return aligns; }
    stats::CharacterStats&      stats() override { return *cs; }

    double health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidHealth) : nullptr;
        return v ? v->value : 0.0;
    }
    double max_health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidMaxHealth) : nullptr;
        return v ? v->value : 0.0;
    }

    CombatWorld&                           world;
    uint64_t                               actor_id;
    npc::MotionStats                       bp;
    std::string                            leaf;
    int                                    lvl;
    std::vector<int64_t>                   aligns;
    std::unique_ptr<stats::CharacterStats> cs;
    npc::ai::NpcAi                         ai;
    bool                                   hostile = false;
    bool                                   dead = false;
    bool                                   flags_in_combat = false;
    uint64_t                               flags_target = 0;
    bool                                   returning = false;
    const npc_kits::Kit*                   kit = nullptr;
    CombatWorld::Attack                    next;
    double                                 next_attack_at = 0;
    std::string                            archetype;
    const std::vector<CreatureAbility>*    anim = nullptr;   // the Blueprint's use-ability tasks when the creature attacks with its own animation
    NpcCastState                           cast;             // the attack the client is shown (combat/npc_ability_replicator.h)
    std::map<std::string, double>          cooldown_until;   // the name of a task's cooldown -> when it is over
    std::vector<int64_t>                   chain;            // the abilities the tree plays next after the cast that just ended (CreatureAbility::next)
    const CreatureAbility*                 chain_from = nullptr;   // the task of that cast: the cooldown that links it to its chain step is open for the step
    double                                 hold_until = 0;   // the creature stands still, facing as it does, until then (the montage of an attack it stands for)
    double                                 attack_walk_share = 1.0;   // how much of its chase speed the tree's walk keeps while the attack it plays goes on (the task's SpeedMultiplierExpression)
    double                                 stand_off_cm = -1;   // a creature whose attacks launch projectiles closes in on its target to this distance (its tree's arrival distance) between two casts; -1: it does not
    double                                 shot_reach_cm = -1;  // ... and starts casting once its target is within this reach (the distance its tree lets it cast from)
    double                                 fight_began = 0;  // when its fight began (the tree asks how long it has gone on)
    double                                 alert_cast_logged_at = -1e9;   // when its last alert cast was named in the log (the per-creature limit of those lines)
    double                                 tree_began = 0;   // when its state tree began to run (the tree asks how long it has run): when the actor was put into the world (NpcInfo::spawned_at; 0 for the creatures the world starts with); a respawn is a new actor, so it starts again
    std::map<std::string, double>          fight_rolls;     // the numbers its tree rolled when this fight began, by the tree's variable (a first special attack comes only after one of them)
    uint64_t                               killer = 0;
    std::set<uint64_t>                     attackers;
    struct Shield {
        double amount = 0;
        double until = 0;   // 0: until used up
        int64_t effect = 0; // the effect whose application added it (0: none; the effect's end or a reset takes it off with it)
        bool by_creature = false;   // a creature's own effect added it: a reset takes it off
    };
    std::vector<Shield>                    shields;
    effects::Book                          book;             // the effects it holds (a burn, a bleed ...): the world runs their hits and shows them as its CurrentEffects
    /// The effects it carries for its tree's "carries effect E" checks: the ones its own abilities put on it and its Blueprint's from spawn (the effect record -> when it ends; 0: it
    /// does not end before the creature does).  Only presence is kept: a mob's own effects do not move its stats yet (the effects another creature's ability puts on it are in `book`).
    std::map<int64_t, double>              own_effects;
    /// The records its Blueprint gave it from spawn (the open's CurrentEffects): a reset keeps them (the records are infinite and persist through death); a fight's own effects go.
    std::set<int64_t>                      starting_effects;
    /// The effects its tree's carries-effect conditions ask about that no source in the world can ever put on it: the tasks that ask them are held (never played), as a condition the
    /// tables cannot say is.
    std::set<int64_t>                      held_effects;

    /// Whether the creature carries an effect record now: its own (see own_effects) or one the world's effects put on it (a player's burn, an ally's buff).
    bool holds_effect(int64_t effect_guid, double now) const {
        const auto it = own_effects.find(effect_guid);
        if (it != own_effects.end() && (it->second <= 0 || now < it->second)) return true;
        return book.stacks_of(effect_guid) > 0;
    }
    /// Takes off its own effects whose time is up (their entry goes; nothing else of the creature changes).
    void expire_own_effects(double now) {
        for (auto it = own_effects.begin(); it != own_effects.end();) it = it->second > 0 && it->second <= now ? own_effects.erase(it) : std::next(it);
    }
    EffectItemIds                          item_ids;         // the CurrentEffects array's item for each effect of the book
    effects::Book::Crowd                   cc;               // the crowd control the book held when the world last wrote it to the creature's state and to its walk
    effects::Book::Crowd                   cc_logged;        // ... and the one the log last named
    /// The place and facing the push that plays works from, worked out when its effect went on (the state names it; a record that caches its origin works from it all along).
    struct PushOrigin {
        bool        set = false;
        OriginFrame frame;
    } push_origin;

    /// A loss goes through the shields first (the oldest first); what is left reaches Health.
    double absorb(double loss, double now) {
        shields.erase(std::remove_if(shields.begin(), shields.end(), [&](const Shield& s) { return s.amount <= 0 || (s.until > 0 && s.until <= now); }), shields.end());
        for (Shield& s : shields) {
            const double take = std::min(s.amount, loss);
            s.amount -= take;
            loss -= take;
        }
        return loss;
    }
};

/// A hit as what reacts to it reads it (the reactions of what the holder holds, and of what the hit carries: combat_pets.cpp).
struct CombatWorld::HolderEvent {
    uint64_t                  attacker = 0;
    uint64_t                  target = 0;   // the actor the hit landed on, when it is the holder's own hit (a reaction bound to the holder's hits reads it)
    const HitOrigin*          origin = nullptr;
    uint8_t                   flags = 0;
    bool                      lethal = false;
    std::map<int64_t, double> mods, effective;
    std::vector<std::pair<const abilities::EffectDef*, uint64_t>> held;   // what the holder held when it landed, and who left it
};

}  // namespace aoc::worldd::combat

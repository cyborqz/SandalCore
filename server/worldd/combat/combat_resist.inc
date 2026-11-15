// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_resist.inc: a creature's crowd control that is resisted (included at the end of combat_world.cpp, inside its namespace)
//
//  The game's UniversalResistChance: the target's EffectEvasion less the caster's EffectAccuracy, a fraction clamped at 0 and rolled in steps of 1/1000.  Only an
//  effect the data tags with CrowdControlConstraint is rolled (a slow, a root, a freeze, a stun, a trip, a sleep, a silence, a disarm); a knock-back, a launch, a
//  pull and a debuff that is not a crowd control carry no such tag and are never resisted.  The target's tier stat is the data's lockout (+100 for 10 s after a
//  kept-up crowd control reaches its build-up's threshold), so the same roll serves both.  A caster with no accuracy (a mob that applied it, a creature whose
//  player is gone) reads 0.
//
//  The data's CharacterType > 1.5 rule (caravans, destructibles, caravels: every debuff resisted) is left for the caravan work.
// ============================================================================

namespace {

/// Whether the effect carries the tag, or a tag below it (the data's EffectHasTag).
bool disable_has_tag(const abilities::EffectDef& def, const char* want) {
    for (const std::string& t : def.row.tags) {
        if (tag_matches(t, want)) return true;
    }
    for (const std::string& t : def.row.effect_tags) {
        if (tag_matches(t, want)) return true;
    }
    return false;
}

/// The aspect the effect's own tags give when the resolver did not work one out (a test's applied effect, an effect whose hit carries no scaling override).
effects::DisableAspect aspect_of_effect_tags(const abilities::EffectDef& def) {
    const bool physical = disable_has_tag(def, "Element.Physical");
    const bool magical = disable_has_tag(def, "Element.Magical");
    if (physical) return effects::DisableAspect::Physical;
    if (magical) return effects::DisableAspect::Magical;
    return effects::DisableAspect::None;
}

}  // namespace

bool CombatWorld::takes_resist_roll(const abilities::EffectDef& def) { return disable_has_tag(def, "CrowdControlConstraint"); }

bool CombatWorld::immunity_buff_holds(const Npc& n) const {
    // the two families' buffs (their data's DisableEvasion +1 is 100 % for every tagged crowd control)
    constexpr int64_t kRafflesiaSteadfastRootBuff = 6064633446201622530ll;
    constexpr int64_t kShardmawVoidRage = 6064634176430277508ll;
    return n.book.stacks_of(kRafflesiaSteadfastRootBuff) > 0 || n.book.stacks_of(kShardmawVoidRage) > 0;
}

bool CombatWorld::effect_resisted(const Npc& n, const abilities::EffectDef& def, const effects::Applied& applied, uint64_t instigator, uint64_t via_pet) {
    if (!takes_resist_roll(def) || !n.cs) return false;
    std::string err;
    auto target = n.cs->evaluation(n.worn(), err, true);
    if (!target) return false;
    const CombatStatIds& id = data_.ids();
    // the data's two terms read their own aspect: EffectEvasion reads the effect's Element tag, EffectAccuracy the effect's tag or the applying
    // hit's scaling override (the resolver works it out).  An effect whose resolver did not say falls back to the effect's own tags.
    const effects::DisableAspect evasion_aspect = aspect_of_effect_tags(def);
    const effects::DisableAspect accuracy_aspect = applied.disable_aspect != effects::DisableAspect::Unset ? applied.disable_aspect : evasion_aspect;
    double evasion = 0;
    if (evasion_aspect == effects::DisableAspect::Physical) evasion += target->stat(id.physical_disable_evasion);
    if (evasion_aspect == effects::DisableAspect::Magical) evasion += target->stat(id.magical_disable_evasion);
    if (disable_has_tag(def, "CrowdControlConstraint.Movement")) {
        if (disable_has_tag(def, "CrowdControlTier.T1")) evasion += target->stat(id.disable_evasion_movement_t1);
        if (disable_has_tag(def, "CrowdControlTier.T2")) evasion += target->stat(id.disable_evasion_movement_t2);
    }
    if (disable_has_tag(def, "CrowdControlConstraint.AbilityActivation")) evasion += target->stat(id.disable_evasion_activation);
    if (disable_has_tag(def, "CrowdControlConstraint.Hard")) {
        if (disable_has_tag(def, "CrowdControlTier.T1")) evasion += target->stat(id.disable_evasion_hard_t1);
        if (disable_has_tag(def, "CrowdControlTier.T2")) evasion += target->stat(id.disable_evasion_hard_t2);
    }
    double accuracy = 0;
    // the data's EffectAccuracy reads the instigator's own stat: a pet's hit is the pet's, although its line is its Summoner's
    Fighter* caster = nullptr;
    if (via_pet != 0) {
        if (const auto it = npcs_.find(via_pet); it != npcs_.end()) caster = it->second.get();
    } else if (PlayerPort* port = player(instigator)) {
        caster = &port->fighter();
    }
    if (caster) {
        auto caster_stats = caster->stats().evaluation(caster->worn(), err, true);
        if (caster_stats) {
            if (accuracy_aspect == effects::DisableAspect::Physical) accuracy += caster_stats->stat(id.physical_disable_accuracy);
            if (accuracy_aspect == effects::DisableAspect::Magical) accuracy += caster_stats->stat(id.magical_disable_accuracy);
        }
    }
    const double chance = std::max(0.0, evasion - accuracy);
    if (!std::isfinite(chance) || chance <= 0.0) return false;
    // the game's integer step: the first non-zero chance is 0.0005 (one step in 1,000); a chance above 1 is every step
    const int steps = static_cast<int>(std::min(1000.0, std::floor(chance * 1000.0 + 0.5)));
    if (steps <= 0) return false;
    const int roll = static_cast<int>(resolver_.random() * 1000.0);
    return roll < steps;
}

void CombatWorld::notify_effect_refused(const Npc& n, const abilities::EffectDef& def, uint64_t instigator, bool immune, double now) {
    PlayerPort* port = player(instigator);
    if (!port) return;
    EffectRefusal refusal;
    refusal.effect_guid = def.row.guid;
    refusal.immune = immune;
    if (const world::WorldActor* a = registry_.find(n.actor_id)) refusal.target = a->actor.guid;
    port->effect_refused(refusal, now);
}

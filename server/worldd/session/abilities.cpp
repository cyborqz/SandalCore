// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/abilities.cpp
//
//  See abilities.h.
// ============================================================================
#include "session/abilities.h"

#include "bootstrap/constants.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aoc { namespace worldd {

using bootstrap::AbilityChange;
using content::AbilityRule;
using content::EffectRule;

const char* refusal_name(Refusal r) {
    switch (r) {
        case Refusal::None: return "none";
        case Refusal::NotEnabled: return "not an ability worldd runs";
        case Refusal::Stamina: return "not enough Stamina";
        case Refusal::Cooldown: return "in cooldown";
        case Refusal::InCombat: return "in combat";
        case Refusal::Duplicate: return "that instance is running";
        case Refusal::NotKnown: return "an ability the character does not know";
        case Refusal::NeedsTarget: return "it needs a target";
        case Refusal::Charges: return "out of charges (its cooldown is running)";
        case Refusal::Cost: return "not enough to pay its cost";
        case Refusal::Busy: return "another ability is being cast";
        case Refusal::Requirement: return "its activation requirement is false";
        case Refusal::Unsupported: return "a shape stage 1 does not run";
        case Refusal::NoFormula: return "one of its formulas does not compile";
        case Refusal::BadTarget: return "no valid target";
        case Refusal::OutOfRange: return "the target is out of range";
        case Refusal::NoLineOfSight: return "the target is out of sight";
        case Refusal::Dead: return "the character is dead";
        case Refusal::Mounted: return "the character is mounted";
    }
    return "?";
}

AbilityRuntime::AbilityRuntime(const content::AbilityRules& rules, std::map<int64_t, double> stats, double world_time_at_zero)
    : rules_(rules), stats_(std::move(stats)), world_time_at_zero_(world_time_at_zero) {
    next_regen_ = rules_.stat_regen_time;
    const auto need = [&](int64_t record, const char* what) {
        if (error_.empty() && stats_.find(record) == stats_.end()) {
            error_ = std::string("the character has no ") + what + " stat (record " + std::to_string(record) + ")";
        }
    };
    need(content::kGuidStamina, "Stamina");
    need(content::kGuidMaxStamina, "MaxStamina");
    tracked_.insert(content::kGuidMaxStamina);
    // The StaminaRegeneration item (the Owner array's): the pawn shows 0 while the regeneration is stopped, as the retail server's does; what it
    // is without a stop is kept in base_, like the stats an effect raises.
    if (const auto regen = stats_.find(content::kGuidStaminaRegeneration); regen != stats_.end()) {
        base_[content::kGuidStaminaRegeneration] = regen->second;
        tracked_.insert(content::kGuidStaminaRegeneration);
    }
    const EffectRule* rest_effect = rules_.effect(rules_.rest.effect_guid);
    if (!rest_effect) {
        error_ = "no Rest effect in the rules";
        return;
    }
    // The stats the Rest's and the Sprint's effects raise (Health and Mana regeneration, MoveSpeedMult): the character must
    // have them, and what they are without the effect is what ending it puts back.
    const EffectRule* sprint_effect = rules_.effect(rules_.sprint.effect_guid);
    if (!sprint_effect) {
        error_ = "no Sprint effect in the rules";
        return;
    }
    for (const EffectRule* e : {rest_effect, sprint_effect}) {
        for (const content::StatMod& m : e->stat_mods) {
            need(m.stat_guid, e == rest_effect ? "regeneration" : "movement speed (MoveSpeedMult)");
            if (m.source_stat_guid != 0) need(m.source_stat_guid, "maximum");
            base_[m.stat_guid] = stat(m.stat_guid);
            tracked_.insert(m.stat_guid);
            if (m.source_stat_guid != 0) tracked_.insert(m.source_stat_guid);
        }
    }
    // The exertion pause is applied as "regeneration stops, standing or moving": the rule rows must say so.
    for (const AbilityRule* a : {&rules_.sprint, &rules_.dodge}) {
        const EffectRule* e = rules_.effect(a->effect_guid);
        const EffectRule* ender = e ? rules_.effect(e->end_effect_guid) : nullptr;
        if (e && e->end_effect_guid != 0 && (!ender || !ender->stops_regen_standing || !ender->stops_regen_moving)) {
            error_ = "the effect that pauses regeneration after " + a->name + " does not stop it while standing and moving";
        }
    }
}
// Cyb : C.O.A

double AbilityRuntime::sync(double t) const { return bootstrap::effect_clock(world_time_at_zero_ + t); }

void AbilityRuntime::emit(AbilityEvent e) {
    if (e.change.empty() && !e.end_ability) return;
    events_.push_back(std::move(e));
}

std::vector<AbilityEvent> AbilityRuntime::take_events() {
    std::vector<AbilityEvent> out;
    out.swap(events_);
    return out;
}

void AbilityRuntime::set_stat(AbilityChange& c, int64_t record, double value) {
    stats_[record] = value;
    const float f = static_cast<float>(value);
    for (bootstrap::StatChange& s : c.stats) {
        if (s.record == record) {
            s.value = f;
            return;
        }
    }
    c.stats.push_back({record, f});
}

bool AbilityRuntime::tracks(int64_t record) const { return tracked_.count(record) != 0; }

void AbilityRuntime::gear_stat(AbilityChange& c, int64_t record, double value) {
    if (record == content::kGuidStamina) return;   // the runtime's own: it changes it, and a smaller MaxStamina clamps it below
    if (record == content::kGuidStaminaRegeneration && base_.count(record) != 0) {
        base_[record] = value;   // the engine's new rate; the pawn shows it unless the regeneration is stopped now
        regen_rate_override_ = value;
        set_stat(c, record, regeneration_blocked(now_) ? 0.0 : value);
        return;
    }
    double shown = value;
    if (const auto base = base_.find(record); base != base_.end()) {
        shown = value + (stat(record) - base->second);   // what an effect adds on now stays on top of the new base
        base->second = value;
    }
    set_stat(c, record, shown);
    if (record == content::kGuidMaxStamina && stat(content::kGuidStamina) > shown) set_stat(c, content::kGuidStamina, shown);
}

void AbilityRuntime::apply_mods(AbilityChange& c, const EffectRule& e) {
    for (const content::StatMod& m : e.stat_mods) {
        const double source = m.source_stat_guid != 0 ? stat(m.source_stat_guid) : 0;
        const auto value = mod_value_.find({e.guid, m.stat_guid});
        set_stat(c, m.stat_guid, base_[m.stat_guid] + m.factor * source + (value != mod_value_.end() ? value->second : m.addend));
    }
}

bool AbilityRuntime::spend_stamina(AbilityChange& c, double amount) {
    if (stamina() < amount) return false;
    set_stat(c, content::kGuidStamina, std::max(0.0, stamina() - amount));
    return true;
}

double AbilityRuntime::add_stamina(AbilityChange& c, double amount) {
    const double cur = stamina();
    const double next = std::clamp(cur + amount, 0.0, std::max(0.0, max_stamina()));
    if (static_cast<float>(next) != static_cast<float>(cur)) set_stat(c, content::kGuidStamina, next);
    return next - cur;
}

void AbilityRuntime::set_effect_value(int64_t effect_guid, int64_t stat_guid, double value, double now) {
    advance(now);
    const auto old = mod_value_.find({effect_guid, stat_guid});
    if (old != mod_value_.end() && old->second == value) return;
    mod_value_[{effect_guid, stat_guid}] = value;
    // a running effect shows the new value at once
    const bool on = (sprint_ && effect_guid == rules_.sprint.effect_guid) || (rest_ && rest_->effect_key && effect_guid == rules_.rest.effect_guid);
    if (!on) return;
    if (const EffectRule* e = rules_.effect(effect_guid)) {
        AbilityChange c;
        apply_mods(c, *e);
        emit({std::move(c), std::nullopt, now_, "an effect's value follows the character's ranks"});
    }
}

void AbilityRuntime::revert_mods(AbilityChange& c, const EffectRule& e) {
    for (const content::StatMod& m : e.stat_mods) set_stat(c, m.stat_guid, base_[m.stat_guid]);
}

bootstrap::EffectItem AbilityRuntime::effect_item(const EffectRule& e, int64_t key, double t) const {
    bootstrap::EffectItem item;
    item.effect_id = key;
    item.effect_guid = e.guid;
    item.start_time = sync(t);
    item.duration = e.infinite ? kInfiniteDuration : static_cast<float>(e.duration);
    item.tick_time = static_cast<float>(e.tick_timer);
    item.stacks = 1;
    return item;
}

bootstrap::ActiveAbilityItem AbilityRuntime::ability_item(const AbilityRule& r, int64_t instance) const {
    bootstrap::ActiveAbilityItem item;
    item.ability_guid = r.guid;
    item.server_instance_id = instance;
    item.targets_self = r.targets_self;
    item.montage_path = r.montage_path;
    return item;
}

double AbilityRuntime::next_due() const {
    double due = next_regen_;
    if (sprint_) due = std::min(due, sprint_->next_charge);
    if (evasion_) due = std::min(due, evasion_->until);
    if (dodge_) due = std::min(due, dodge_->until);
    if (pause_effect_) due = std::min(due, pause_effect_->until);
    if (rest_ && rest_->effect_due) due = std::min(due, *rest_->effect_due);
    if (rest_ && rest_->remove_due) due = std::min(due, *rest_->remove_due);
    for (const PendingEnd& e : pending_ends_) due = std::min(due, e.due);
    return due;
}

void AbilityRuntime::advance(double now) {
    if (!ok()) return;
    if (now < now_) now = now_;   // the clock never goes back
    for (;;) {
        const double t = next_due();
        if (t > now) break;
        run_due(t);
    }
    now_ = now;
}

void AbilityRuntime::run_due(double t) {
    if (sprint_ && sprint_->next_charge <= t) return charge_sprint(t);
    for (auto it = pending_ends_.begin(); it != pending_ends_.end(); ++it) {
        if (it->due > t) continue;
        AbilityEvent e;
        e.end_ability = it->client_id;
        e.at = t;
        e.note = "the client is told the Sprint is over";
        pending_ends_.erase(it);
        return emit(std::move(e));
    }
    if (evasion_ && evasion_->until <= t) return end_evasion(t);
    if (dodge_ && dodge_->until <= t) return end_dodge_items(t);
    if (pause_effect_ && pause_effect_->until <= t) return end_pause_effect(t);
    if (rest_ && rest_->effect_due && *rest_->effect_due <= t) return add_rest_effect(t);
    if (rest_ && rest_->remove_due && *rest_->remove_due <= t) return remove_rest_effect(t);
    if (next_regen_ <= t) return regenerate(t);
}
// [Cyb ~ C O A]

void AbilityRuntime::tick(double now) { advance(now); }

void AbilityRuntime::set_mounted(bool mounted, double now) {
    if (!ok()) return;
    advance(now);
    if (mounted_ == mounted) return;
    mounted_ = mounted;
    if (!mounted_) return;
    if (sprint_) end_sprint(now_, Tell::Now, "the Sprint ends: the player gets on a mount", /*exertion=*/false);
    if (rest_ && !rest_->ended) release_rest(now_, "the Rest ends: the player gets on a mount");
    moving_ = false;
}

void AbilityRuntime::set_moving(bool moving, double now) {
    advance(now);
    if (mounted_) moving = false;   // a rider's moves are its mount's: nothing it does charges the pawn
    moving_ = moving;
    moving_at_ = std::max(now, now_);
    // the Sprint's regeneration stop depends on the player moving: the StaminaRegeneration item follows
    if (sprint_ && base_.count(content::kGuidStaminaRegeneration) != 0) {
        AbilityChange c;
        sync_regen(c, now_);
        emit({std::move(c), std::nullopt, now_, "the player starts or stops moving in a Sprint: the Stamina regeneration follows"});
    }
}

void AbilityRuntime::sync_regen(AbilityChange& c, double t) {
    const auto base = base_.find(content::kGuidStaminaRegeneration);
    if (base == base_.end()) return;
    const double want = regeneration_blocked(t) ? 0.0 : base->second;
    if (static_cast<float>(stat(content::kGuidStaminaRegeneration)) != static_cast<float>(want)) set_stat(c, content::kGuidStaminaRegeneration, want);
}

void AbilityRuntime::set_regen_rate(double per_second, double now) {
    advance(now);
    regen_rate_override_ = per_second;
    const auto base = base_.find(content::kGuidStaminaRegeneration);
    if (base == base_.end() || base->second == per_second) return;
    base->second = per_second;
    AbilityChange c;
    sync_regen(c, now_);
    emit({std::move(c), std::nullopt, now_, "the Stamina regeneration rate changes"});
}

void AbilityRuntime::set_in_combat(bool in_combat, double now) {
    advance(now);
    in_combat_ = in_combat;
}

bool AbilityRuntime::regeneration_blocked(double now) const {
    if (sprint_) {
        const EffectRule* e = rules_.effect(rules_.sprint.effect_guid);
        if (e && (moving(now) ? e->stops_regen_moving : e->stops_regen_standing)) return true;
    }
    if (evasion_) {
        const EffectRule* e = rules_.effect(rules_.dodge.effect_guid);
        if (e && (moving(now) ? e->stops_regen_moving : e->stops_regen_standing)) return true;
    }
    return now < pause_until_;
}

void AbilityRuntime::pause_regeneration(AbilityChange& c, const EffectRule& owner, double t) {
    if (owner.end_effect_guid == 0) return;
    const EffectRule* pause = rules_.effect(owner.end_effect_guid);
    if (!pause) return;
    pause_until_ = std::max(pause_until_, t + owner.end_effect_duration);
    if (pause_effect_) c.effects_removed.push_back(pause_effect_->effect_key);   // a new pause replaces the running one
    const int64_t key = next_effect_id();
    bootstrap::EffectItem item = effect_item(*pause, key, t);
    item.duration = static_cast<float>(pause_until_ - t);
    c.effects_added.push_back(item);
    pause_effect_ = Pause{key, pause_until_};
}

void AbilityRuntime::end_pause_effect(double t) {
    AbilityChange c;
    c.effects_removed.push_back(pause_effect_->effect_key);
    pause_effect_.reset();
    sync_regen(c, t);   // retail: StaminaRegeneration 5.0 in the packet that ends the pause
    emit({std::move(c), std::nullopt, t, "the regeneration pause ends"});
}

void AbilityRuntime::regenerate(double t) {
    next_regen_ = t + rules_.stat_regen_time;
    const double max = max_stamina();
    if (stamina() >= max || regeneration_blocked(t)) return;
    AbilityChange c;
    set_stat(c, content::kGuidStamina, std::min(max, stamina() + regen_rate_override_.value_or(rules_.regen_rate) * rules_.stat_regen_time));
    emit({std::move(c), std::nullopt, t, "Stamina regenerates"});
}

void AbilityRuntime::refuse(int32_t client_instance_id, Refusal why, double t, const char* what) {
    ++refused_;
    AbilityEvent e;
    e.end_ability = client_instance_id;
    e.at = t;
    e.note = std::string(what) + " refused: " + refusal_name(why);
    e.refusal = true;
    events_.push_back(std::move(e));
}

std::optional<int64_t> AbilityRuntime::instance_of(int32_t client_instance_id) const {
    if (sprint_ && sprint_->client_id == client_instance_id) return sprint_->instance;
    if (dodge_ && dodge_->client_id == client_instance_id) return dodge_->instance;
    if (rest_ && rest_->client_id == client_instance_id) return rest_->instance;
    return std::nullopt;
}

Refusal AbilityRuntime::trigger(int64_t ability_guid, int32_t client_instance_id, double now, const TriggerInfo& info) {
    if (!ok()) return Refusal::NotEnabled;
    advance(now);
    const AbilityRule* r = rules_.ability(ability_guid);
    if (!r) {
        refuse(client_instance_id, Refusal::NotEnabled, now_, "an ability worldd does not run");
        return Refusal::NotEnabled;
    }
    // An instance that is running is not started twice, and not ended either: ClientEndAbility(id) would end the real one.
    if (instance_of(client_instance_id)) return Refusal::Duplicate;
    if (mounted_) {
        refuse(client_instance_id, Refusal::Mounted, now_, r->name.c_str());
        return Refusal::Mounted;
    }
    if (r->requires_out_of_combat && in_combat_) {
        refuse(client_instance_id, Refusal::InCombat, now_, r->name.c_str());
        return Refusal::InCombat;
    }
    if (r == &rules_.sprint) {
        if (stamina() < r->min_stamina) {
            refuse(client_instance_id, Refusal::Stamina, now_, r->name.c_str());
            return Refusal::Stamina;
        }
        start_sprint(client_instance_id, now_);
    } else if (r == &rules_.dodge) {
        if (r->disable_during_cooldown && now_ < dodge_cooldown_until_) {
            refuse(client_instance_id, Refusal::Cooldown, now_, r->name.c_str());
            return Refusal::Cooldown;
        }
        if (stamina() < r->cost_amount) {
            refuse(client_instance_id, Refusal::Stamina, now_, r->name.c_str());
            return Refusal::Stamina;
        }
        start_dodge(client_instance_id, now_, info);
    } else {
        start_rest(client_instance_id, now_);
    }
    return Refusal::None;
}
// CoA captures, C | Y | B code

bool AbilityRuntime::cancel(int32_t client_instance_id, double now) {
    if (!ok()) return false;
    advance(now);
    if (sprint_ && sprint_->client_id == client_instance_id) {
        end_sprint(now_, Tell::No, "the Sprint is released");
        return true;
    }
    if (rest_ && !rest_->ended && rest_->client_id == client_instance_id) {
        release_rest(now_, "the Rest is released");
        return true;
    }
    return false;
}

// ---- Sprint ---------------------------------------------------------------------------------------------------------

bool AbilityRuntime::take_charge(AbilityChange& c) {
    const AbilityRule& r = rules_.sprint;
    const double before = stamina();
    const double after = std::max(0.0, before - r.cost_amount);
    if (after != before) set_stat(c, r.cost_stat_guid, after);
    return after < r.min_stamina;
}

void AbilityRuntime::start_sprint(int32_t id, double t) {
    if (sprint_) end_sprint(t, Tell::No, "the Sprint is replaced");
    if (rest_ && !rest_->ended) release_rest(t, "the Rest ends: a Sprint starts");
    const AbilityRule& r = rules_.sprint;
    const EffectRule& e = *rules_.effect(r.effect_guid);
    const int64_t instance = next_instance_++;
    AbilityChange c;
    bootstrap::ActiveAbilityItem item = ability_item(r, instance);
    item.montage_group = std::string(bootstrap::kSprintMontageGroup);
    c.abilities_added.push_back(std::move(item));
    const int64_t key = next_effect_id();
    c.effects_added.push_back(effect_item(e, key, t));
    apply_mods(c, e);   // MoveSpeedMult + 0.2: the client moves at 600 cm/s times it
    sprint_ = Sprint{id, instance, key, t + r.charge_interval};
    // The first charge is taken with the activation, while the player moves (retail: the first Stamina change comes with the
    // reply, median 0 ms after it, 33 of 50 within 200 ms); then every interval.
    bool out_of_stamina = false;
    if (!r.cost_requires_moving || moving(t)) out_of_stamina = take_charge(c);
    sync_regen(c, t);   // the regeneration stops with the Sprint while the player moves: the StaminaRegeneration item says 0 in the same packet as the first charge
    emit({std::move(c), std::nullopt, t, "the Sprint starts"});
    if (out_of_stamina) end_sprint(t, Tell::Later, "the Sprint ends: out of Stamina");
}

void AbilityRuntime::end_sprint(double t, Tell tell, const char* why, bool exertion) {
    if (!sprint_) return;
    const EffectRule& e = *rules_.effect(rules_.sprint.effect_guid);
    AbilityChange c;
    c.abilities_removed.push_back(sprint_->instance);
    c.effects_removed.push_back(sprint_->effect_key);
    revert_mods(c, e);   // MoveSpeedMult back to what it was: 1.0
    const int32_t id = sprint_->client_id;
    sprint_.reset();
    if (exertion) pause_regeneration(c, e, t);   // the DisableRegen effect, in the same step (retail: the same packet as MoveSpeedMult 1.0)
    sync_regen(c, t);
    AbilityEvent ev{std::move(c), std::nullopt, t, why};
    if (tell == Tell::Now) ev.end_ability = id;
    emit(std::move(ev));
    if (tell == Tell::Later) pending_ends_.push_back({id, t + kEndAbilityDelay});
}

void AbilityRuntime::charge_sprint(double t) {
    const AbilityRule& r = rules_.sprint;
    sprint_->next_charge = t + r.charge_interval;
    AbilityChange c;
    if (r.cost_requires_moving && !moving(t)) {   // the cost group is enabled only while moving
        sync_regen(c, t);   // (a player that stopped moving without a move to say so: the regeneration is not stopped any more)
        emit({std::move(c), std::nullopt, t, "the Sprint's player stands: the Stamina regeneration follows"});
        return;
    }
    const bool out_of_stamina = take_charge(c);
    sync_regen(c, t);
    emit({std::move(c), std::nullopt, t, "the Sprint charges"});
    if (out_of_stamina) end_sprint(t, Tell::Later, "the Sprint ends: out of Stamina");
}

// ---- Dodge ----------------------------------------------------------------------------------------------------------

void AbilityRuntime::start_dodge(int32_t id, double t, const TriggerInfo& info) {
    // A Dodge does not end a running Sprint (retail capture 745: the Sprint, MoveSpeedMult and the drain run on).
    if (rest_ && !rest_->ended) release_rest(t, "the Rest ends: a Dodge starts");
    const AbilityRule& r = rules_.dodge;
    const EffectRule& e = *rules_.effect(r.effect_guid);
    AbilityChange c;
    if (evasion_) {
        c.effects_removed.push_back(evasion_->effect_key);
        evasion_.reset();
    }
    if (dodge_) {
        c.abilities_removed.push_back(dodge_->instance);
        dodge_.reset();
    }
    set_stat(c, r.cost_stat_guid, stamina() - r.cost_amount);
    const int64_t instance = next_instance_++;
    bootstrap::ActiveAbilityItem item = ability_item(r, instance);
    // The montage the client's AnimationIndex names, and the direction of the input (the rule's own montage is the first
    // of the ability's list, the Rogue's forward dash: not what a Bard plays)
    const std::optional<bootstrap::DodgeMontage> montage = bootstrap::dodge_montage_for(info.animation_index);
    item.montage_path = montage ? std::string(montage->path) : std::string();
    item.mesh_yaw = montage ? bootstrap::dodge_mesh_yaw(*montage, info.input_x, info.input_y) : 0;
    c.abilities_added.push_back(std::move(item));
    double items_until = t;
    if (r.triggers_cooldown && r.cooldown > 0) {
        bootstrap::CooldownItem cd;
        cd.ability_guid = r.guid;
        cd.triggered = true;
        cd.initial_cooldown = static_cast<float>(r.cooldown);
        cd.start_time = sync(t);
        cd.end_time = cd.start_time + static_cast<double>(cd.initial_cooldown);   // retail: the end is the start plus the float
        cd.charges = 1;
        c.cooldowns_set.push_back(cd);
        dodge_cooldown_until_ = t + r.cooldown;
        items_until = dodge_cooldown_until_;
    }
    const int64_t key = next_effect_id();
    c.effects_added.push_back(effect_item(e, key, t));
    evasion_ = Evasion{key, t + e.duration};
    dodge_ = Dodge{id, instance, std::max(items_until, evasion_->until)};
    sync_regen(c, t);   // the Evasion stops the regeneration: the StaminaRegeneration item says 0 with the Dodge's packet
    dash_until_ = std::max(dash_until_, evasion_->until + kDashMargin);
    emit({std::move(c), std::nullopt, t, "the Dodge starts"});
}

void AbilityRuntime::end_evasion(double t) {
    AbilityChange c;
    c.effects_removed.push_back(evasion_->effect_key);
    evasion_.reset();
    pause_regeneration(c, *rules_.effect(rules_.dodge.effect_guid), t);   // retail: the DisableRegen effect replaces the Evasion
    sync_regen(c, t);
    emit({std::move(c), std::nullopt, t, "the Evasion ends"});
}

void AbilityRuntime::end_dodge_items(double t) {
    AbilityChange c;
    c.abilities_removed.push_back(dodge_->instance);
    if (rules_.dodge.triggers_cooldown && rules_.dodge.cooldown > 0) c.cooldowns_removed.push_back(rules_.dodge.guid);
    dodge_.reset();
    emit({std::move(c), std::nullopt, t, "the Dodge ends: its cooldown is over"});
}

// ---- Rest -----------------------------------------------------------------------------------------------------------
// Cyb - C | O | A

void AbilityRuntime::start_rest(int32_t id, double t) {
    // No retail capture has a Rest, so what the client does with its own Sprint is unknown: the server ends the Sprint and tells it.
    if (sprint_) end_sprint(t, Tell::Now, "the Sprint ends: a Rest starts");
    const AbilityRule& r = rules_.rest;
    AbilityChange c;
    if (rest_) {
        // The running (or released) instance is replaced in one step, as pkt#8240: its effect goes with the regeneration it
        // raised, its item goes, the new item's AnimationCounter is two on (the montage restarted: pkt#8240's 2).
        if (rest_->effect_key) {
            c.effects_removed.push_back(*rest_->effect_key);
            revert_mods(c, *rules_.effect(r.effect_guid));
        }
        c.abilities_removed.push_back(rest_->instance);
        rest_animation_counter_ += 2;
    }
    const int64_t instance = next_instance_++;
    bootstrap::ActiveAbilityItem item = ability_item(r, instance);
    item.animation_counter = rest_animation_counter_;
    c.abilities_added.push_back(item);
    rest_ = Rest{id, instance};
    rest_->effect_due = t + kRestEffectDelay;
    emit({std::move(c), std::nullopt, t, "the Rest starts"});
}

void AbilityRuntime::release_rest(double t, const char* why) {
    if (!rest_ || rest_->ended) return;
    rest_->ended = true;
    rest_->effect_due.reset();
    // The montage jumps to its end section (pkt#8318); the effect follows kRestEndEffectDelay later (pkt#8320).
    bootstrap::ActiveAbilityItem item = ability_item(rules_.rest, rest_->instance);
    item.montage_position = kRestSitEndPosition;
    item.may_jump_to_position = true;
    item.animation_counter = ++rest_animation_counter_;
    if (rest_->effect_key) rest_->remove_due = t + kRestEndEffectDelay;
    AbilityChange c;
    c.abilities_changed.push_back(item);
    emit({std::move(c), std::nullopt, t, why});
}

void AbilityRuntime::add_rest_effect(double t) {
    rest_->effect_due.reset();
    const EffectRule& e = *rules_.effect(rules_.rest.effect_guid);
    AbilityChange c;
    const int64_t key = next_effect_id();
    c.effects_added.push_back(effect_item(e, key, t));
    apply_mods(c, e);
    rest_->effect_key = key;
    emit({std::move(c), std::nullopt, t, "the Rest effect starts"});
}

void AbilityRuntime::remove_rest_effect(double t) {
    rest_->remove_due.reset();
    if (!rest_->effect_key) return;
    AbilityChange c;
    c.effects_removed.push_back(*rest_->effect_key);
    rest_->effect_key.reset();
    revert_mods(c, *rules_.effect(rules_.rest.effect_guid));
    emit({std::move(c), std::nullopt, t, "the Rest effect ends"});
}

}}  // namespace aoc::worldd

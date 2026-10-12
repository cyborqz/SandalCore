// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/ability_pipeline.cpp  (see ability_pipeline.h)
// ============================================================================
#include "session/ability_pipeline.h"

#include "bootstrap/constants.h"
#include "content/content_db.h"
#include "session/ability_timings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using bootstrap::AbilityChange;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// The one class resource the evidence shows: a Bard's Resonance (an effect that stacks to 10 for 30 s).
struct ClassResource {
    int         class_enum;
    const char* effect;
};
constexpr ClassResource kClassResources[] = {{2, "Bard_Resonance"}};

// The abilities whose cost step comes later than the montage's first notify (retail: the ability item at `item`, the cost, cooldown and
// resonance at `cost`; capture 44 and 36-38).
struct Timing {
    const char* ability;
    double      item, cost;
    double      hit;   // the ability's own hits come this long after the cost step (-1: kHitDelay)
};
constexpr Timing kTimings[] = {
    {"Bard_Dance_Mesmerizing", 0.323, 1.011, -1},   // capture 44: item +323 ms, cost +1011
    {"Bard_Saga", 0.315, 0.906, -1},                // item +315, cost +906
    {"Bard_Story_Joy", 0.312, 1.573, 0.0},          // item +312, cost +1573, its effects with it
    {"Bard_Song_Anthem", 0.303, 1.904, -1},         // item +303, cost +1904
    {"Bard_Song_Serenade", 0.303, 0.303, 0.307},    // item and cost +303, the Spirited buff +610
};

// The speed (cm/s) the retail client's own moves reached while an ability played, for the abilities whose data has no NetworkingMaxSpeed:
// Flourish's slide, 3,659 cm/s at its fastest in both casts of capture 44 (decaying to 0 in 0.6 s).
struct Dash {
    const char* ability;
    double      speed;
};
// The other movement abilities have no measured speed, only the displacement the client's own moves covered within 1.5 s of the cast (research of
// the all-classes captures: Ranger_Disengage up to 1,578 cm, Tank_Charge 3,194 [the brief: up to 32 m], Tank_Grapple 1,027, Summoner_Transpose 1,231;
// Rogue_Shadowstep (a teleport to its target) has no timestamps): the speed [modelled] is that displacement times Flourish's ratio of peak speed to
// displacement (3,659 / 2,319 = 1.58).
constexpr Dash kDashes[] = {{"Bard_Misc_Flourish", 3659.0}, {"Ranger_Disengage", 2500.0}, {"Tank_Charge", 5050.0}, {"Tank_Grapple", 1650.0},
                            {"Summoner_Transpose", 1950.0}, {"Rogue_Shadowstep", 3000.0}};

// The MontageGroup of an ability's item where it is not "DefaultGroup" (retail: Flourish EmoteGroup, the five songs SongGroup).
std::string_view group_of_name(std::string_view name) {
    if (name == "Bard_Misc_Flourish") return "EmoteGroup";
    if (name.rfind("Bard_Music_", 0) == 0) return "SongGroup";
    return "DefaultGroup";
}
// C O A captures, Cyb code

bool is_none_tag(const std::string& s) { return s.empty() || s == "None"; }

std::string group_key(const content::AbilityRow& row) { return is_none_tag(row.shared_cooldown_tag) ? std::to_string(row.guid) : row.shared_cooldown_tag; }

const char* const kExpirePhases[] = {"Expire", "End"};
const char* const kTickPhases[] = {"Tick"};
const char* const kApplyPhases[] = {"Apply"};

// ---- E1 stage 3 fix 2: a weapon combo's step --------------------------------------------------------------------------------------------------
//
// A ranged weapon combo's projectile hits are the server's to resolve (retail: 1,272 book, 77 longbow and 28 shortbow swings, not one
// RequestHitVerification; one confirmation 0.5-1.3 s after the trigger), and the swing fires the projectile of ITS STEP: the client's AnimationIndex names
// the step's montage, and the montage's name carries the step the way the hits' names do (WeaponCombo_RNG_BOOK_03_Proc and Weapon_Book_Hit_3+,
// WeaponCombo_Longbow_04+_Montage and WeaponCombo_Longbow_04+_Proc, WeaponCombo_RngWnd_C_05 and Weapon_Wand_Hit_5; the first swing's montage may carry no
// number).  The book's damage is not in its projectile's hit (Weapon_Book_Hit_n's stat mod is the data's "0 ... here for the primary FX ... not to do
// damage") but in Book_Projectile_Split_Hit_n, a direct hit no projectile carries that fires the split projectile when it lands: retail confirms the two
// together, in one confirmation, the FX hit predicted on both sides and the Split hit on none [R: research/hitfx, 358 of 358 book confirmations].
struct StepToken {
    bool ok = false;
    int  step = 1;
    bool proc = false;      // the "+" / "_Proc" variant (the proc'd finisher)
    int  variant = 0;       // "_2": the second projectile of the step
    bool finisher = false;  // "_Finisher"
    bool beam = false;      // the wand's beam montages: no projectile (the client asks for the beam hits)
};

/// The step of a montage's or a hit's name: the last token that holds digits, the "+" after them, "_2" after that, "Proc" and "Finisher" words.
StepToken step_token(std::string_view name) {
    StepToken t;
    if (name.rfind("DEPR_", 0) == 0) return t;
    if (name.size() > 8 && name.compare(name.size() - 8, 8, "_Montage") == 0) name.remove_suffix(8);
    std::vector<std::string_view> tokens;
    for (size_t start = 0; start <= name.size();) {
        const size_t end = name.find('_', start);
        tokens.push_back(name.substr(start, (end == std::string_view::npos ? name.size() : end) - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    int last_digits = -1;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string_view tok = tokens[i];
        if (tok == "Proc") t.proc = true;
        if (tok == "Finisher") t.finisher = true;
        if (tok == "Beam" || tok == "Beam2") t.beam = true;
        const size_t d = tok.find_first_of("0123456789");
        if (d == std::string_view::npos) continue;
        if (tok == "2" && last_digits >= 0 && i == static_cast<size_t>(last_digits) + 1) {   // "_4_2": the second projectile of step 4
            t.variant = 2;
            continue;
        }
        int n = 0;
        size_t j = d;
        while (j < tok.size() && tok[j] >= '0' && tok[j] <= '9') n = n * 10 + (tok[j++] - '0');
        t.step = n;
        t.proc = t.proc || (j < tok.size() && tok[j] == '+');
        last_digits = static_cast<int>(i);
    }
    t.ok = true;
    return t;
}

bool same_step(const StepToken& a, const StepToken& b) { return a.ok && b.ok && a.step == b.step && a.proc == b.proc; }

std::string_view leaf_of_path(const std::string& path) {
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    const size_t start = std::max(dot == std::string::npos ? 0 : dot + 1, slash == std::string::npos ? 0 : slash + 1);
    return std::string_view(path).substr(start);
}

bool is_weapon_combo(const content::AbilityRow& row) {
    for (const std::string& t : row.tags) {
        if (t == "ability.weaponcombo") return true;
    }
    return row.name.rfind("WeaponCombo_", 0) == 0;
}

}  // namespace

// What a program asks of the host besides the stats: the instigator's effect counts, a seeded random number, nothing of a running ability's
// progress (songs are stage 2) or of a hit event (the defaults).
class AbilityPipeline::Host final : public combat::ContextHost {
public:
    explicit Host(AbilityPipeline& p) : p_(p) {}
    double effect_count_by_instigator(const stats::RecordRef& r) override { return p_.stacks_of(r.guid); }
    double rand_double() override { return p_.rng_.rand_double(); }
    double ability_progress(const std::string& group) override { return p_.progress_of(group); }

protected:
    // the named host functions (statprog's Host... constants) the hit in progress does not answer (combat/hit_context.h does the damage functions, the
    // roles' distance and identity and the hit's own tags and flags): the cast's own state; with only the caster in its world every target is the caster
    double other_fn(int fn, const std::string& name, const double* args, int) override {
        switch (fn) {
            case stats::kHostRemainingCharges: return p_.remaining_charges(std::strtoll(name.c_str(), nullptr, 10));
            case stats::kHostApplicableTarget: {
                // with another actor as the target (a hit or a requirement that has one): its side of the caster's relations; else the target is the caster
                if (ctx_ && ctx_->target) {
                    if (name == "TARGET_Enemy") return ctx_->enemy ? 1.0 : 0.0;
                    if (name == "TARGET_Neutral") return ctx_->neutral ? 1.0 : 0.0;
                    if (name == "TARGET_Self") return 0.0;
                    return (!ctx_->enemy && !ctx_->neutral) ? 1.0 : 0.0;   // Ally, Party, Raid: its own side
                }
                return name == "TARGET_Self" ? 1.0 : 0.0;
            }
            case stats::kHostMovementKey: return p_.moving_ ? 1.0 : 0.0;
            case stats::kHostStatDelta: return p_.stat_delta_over_time(std::strtoll(name.c_str(), nullptr, 10), args[0]);
            case stats::kHostNow: return p_.clock_;
            case stats::kHostProgressDuration: return name == "DefaultGroup" ? kCastSeconds : 0.0;
            case stats::kHostRandIntRange: {
                const double lo = std::floor(args[0]), hi = std::floor(args[1]);
                return hi < lo ? lo : lo + std::floor(p_.rng_.rand_double() * (hi - lo + 1.0));
            }
            default: return 0.0;   // the hit event's stat mods, an angle: nothing is there
        }
    }

private:
    AbilityPipeline& p_;
};
// -- C-Y-B & C-O-A --

AbilityPipeline::AbilityPipeline(const abilities::Catalog* catalog, stats::CharacterStats& stats, AbilityRuntime& a7, WornFn worn, double world_time_at_zero,
                                 uint64_t seed)
    : catalog_(catalog), stats_(stats), a7_(a7), worn_(std::move(worn)), world_time_at_zero_(world_time_at_zero) {
    rng_.state = seed;
    stats_.set_resources(std::set<int64_t>(std::begin(content::kResourceStats), std::end(content::kResourceStats)));
    resolve_resource();
}

void AbilityPipeline::resolve_resource() {
    resource_ = nullptr;
    resource_class_ = stats_.class_enum();
    if (!catalog_) return;
    for (const ClassResource& r : kClassResources) {
        if (r.class_enum == stats_.class_enum()) resource_ = catalog_->effect_named(r.effect);
    }
}

void AbilityPipeline::set_known(std::vector<int64_t> known) { known_ = std::set<int64_t>(known.begin(), known.end()); }

bool AbilityPipeline::handles(int64_t ability) const { return catalog_ && catalog_->ability(ability) != nullptr && !a7_.enabled(ability); }

double AbilityPipeline::sync(double t) const { return bootstrap::effect_clock(world_time_at_zero_ + t); }

void AbilityPipeline::emit(AbilityEvent e) {
    // the hits this step made are confirmed with it (the client draws its floating numbers from them)
    for (bootstrap::HitConfirm& h : confirmations_) e.confirmations.push_back(std::move(h));
    confirmations_.clear();
    if (e.change.empty() && !e.end_ability && e.confirmations.empty() && e.zones.empty()) return;
    events_.push_back(std::move(e));
}

std::vector<AbilityEvent> AbilityPipeline::take_events() {
    std::vector<AbilityEvent> out;
    out.swap(events_);
    return out;
}

void AbilityPipeline::refuse(int32_t client_id, Refusal why, double t, const std::string& what, bool) {
    ++refused_;
    AbilityEvent e;
    e.end_ability = client_id;
    e.at = t;
    e.note = what + " refused: " + refusal_name(why);
    e.refusal = true;
    events_.push_back(std::move(e));
}

// ---- evaluation -----------------------------------------------------------------------------------------------------------------------

std::string AbilityPipeline::expr_problem(int64_t expr) const {
    const content::ExprRow* e = catalog_ ? catalog_->expr(expr) : nullptr;
    if (!e) return "no expression " + std::to_string(expr);
    return e->error.empty() ? "expression " + std::to_string(expr) + " has no program" : e->error;
}

bool AbilityPipeline::eval(int64_t expr, double& out, std::string& why) {
    if (!catalog_) {
        why = "no ability tables";
        return false;
    }
    const content::ExprRow* e = catalog_->expr(expr);
    if (!e || e->program < 0) {
        why = expr_problem(expr);
        return false;
    }
    if (!ensure_evaluation(why)) return false;
    Host host(*this);
    std::string err;
    double v = 0;
    if (!evaluation_->eval(e->program, host, v, err)) {
        why = err;
        return false;
    }
    if (!std::isfinite(v)) {
        why = "expression " + std::to_string(expr) + " is not a finite number";
        return false;
    }
    out = v;
    return true;
}

bool AbilityPipeline::ensure_evaluation(std::string& why) {
    if (evaluation_) return true;
    std::string err;
    evaluation_ = stats_.evaluation(worn_(), err);
    if (!evaluation_) {
        why = "the stat engine cannot run: " + err;
        return false;
    }
    return true;
}

bool AbilityPipeline::eval_bool(int64_t expr, bool none, bool& out, std::string& why) {
    if (expr == 0) {
        out = none;
        return true;
    }
    double v = 0;
    if (!eval(expr, v, why)) return false;
    out = v != 0;
    return true;
}
// [C O A :: Cyb]

bool AbilityPipeline::eval_requirement(int64_t expr, const std::vector<uint64_t>& targets, bool& out, std::string& why) {
    if (expr == 0 || targets.empty() || !combat_) return eval_bool(expr, true, out, why);
    const content::ExprRow* e = catalog_ ? catalog_->expr(expr) : nullptr;
    if (!e || e->program < 0) {
        why = expr_problem(expr);
        return false;
    }
    const combat::TargetInfo ti = combat_->inspect(targets[0]);
    combat::Fighter* tf = ti.exists ? combat_->fighter(targets[0]) : nullptr;
    if (!tf) return eval_bool(expr, true, out, why);   // (an actor that is none is the cast's own validation's to refuse)
    if (!ensure_evaluation(why)) return false;
    std::string err;
    auto target_eval = tf->stats().evaluation(tf->worn(), err, true);
    if (!target_eval) return eval_bool(expr, true, out, why);
    const combat::EvalView self_view(*evaluation_), target_view(*target_eval);
    combat::HitContext ctx;
    ctx.instigator = &self_view;
    ctx.target = &target_view;
    ctx.distance_cm = ti.surface_distance_cm + ti.radius_cm + combat_->self().radius_cm();   // (GetDistanceBetween: centre to centre)
    ctx.enemy = ti.relation == combat::Relation::Hostile;
    ctx.neutral = ti.relation == combat::Relation::Neutral;
    Host host(*this);
    host.set_context(&ctx);
    double v = 0;
    if (!evaluation_->eval(e->program, host, v, err)) {
        why = err;
        return false;
    }
    out = v != 0;
    return true;
}

// ---- stats ----------------------------------------------------------------------------------------------------------------------------

double AbilityPipeline::stat_now(int64_t guid) const {
    if (a7_.tracks(guid)) return a7_.stat(guid);
    const stats::StatValue* v = stats_.find(guid);
    return v ? static_cast<double>(v->value) : 0.0;
}

void AbilityPipeline::set_held(AbilityChange& c, int64_t stat, double value) {
    const float f = static_cast<float>(value);
    stats_.set_current(stat, f);
    for (bootstrap::StatChange& s : c.stats) {
        if (s.record == stat) {
            s.value = f;
            return;
        }
    }
    bootstrap::StatChange sc;
    sc.record = stat;
    sc.value = f;
    if (const stats::StatValue* v = stats_.find(stat)) {
        sc.buckets = v->buckets();
        sc.base = v->base;
        sc.equipment = v->equip;
    }
    c.stats.push_back(sc);
}

bool AbilityPipeline::take_stat(AbilityChange& c, int64_t stat, double amount) {
    if (stat == content::kGuidStamina) return a7_.spend_stamina(c, amount);
    const double cur = stat_now(stat);
    if (cur < amount) return false;
    set_held(c, stat, std::max(0.0, cur - amount));
    return true;
}

std::vector<stats::ActiveEffect> AbilityPipeline::engine_effects() const {
    std::vector<stats::ActiveEffect> out;
    out.reserve(effects_.size());
    for (const EffectInst& e : effects_) {
        stats::ActiveEffect a;
        a.effect = {stats::kRefTypeEffect, e.def->row.guid};
        a.stacks = e.stacks;
        a.tags = e.def->row.tags;
        a.tags.insert(a.tags.end(), e.def->row.effect_tags.begin(), e.def->row.effect_tags.end());
        a.mods = e.mods;
        out.push_back(std::move(a));
    }
    return out;
}

void AbilityPipeline::recompute(AbilityChange& c) {
    drop_evaluation();
    sync_shields(c);
    if (!stats_.started()) return;
    stats_.set_effects(engine_effects());
    std::vector<stats::StatValue> changed;
    stats::CharacterStats::Next next;
    std::string err;
    if (!stats_.prepare(worn_(), changed, next, err)) return;
    stats_.commit(std::move(next));
    for (const stats::StatValue& v : changed) {
        if (v.float_array() || (v.replication != 1 && v.replication != 5 && v.replication != 7)) continue;
        if (v.guid == content::kGuidStamina) continue;   // A7's own
        if (a7_.tracks(v.guid)) {
            a7_.gear_stat(c, v.guid, v.value);   // an effect of A7's that raises it keeps its raise on top of the new value
            continue;
        }
        bool done = false;
        for (bootstrap::StatChange& s : c.stats) {
            if (s.record != v.guid) continue;
            s.value = v.value;
            s.buckets = v.buckets();
            s.base = v.base;
            s.equipment = v.equip;
            done = true;
        }
        if (done) continue;
        bootstrap::StatChange sc;
        sc.record = v.guid;
        sc.value = v.value;
        sc.buckets = v.buckets();
        sc.base = v.base;
        sc.equipment = v.equip;
        c.stats.push_back(sc);
    }
}

bool AbilityPipeline::grant_effect(int64_t effect_guid, int stacks, double now) {
    const EffectDef* d = catalog_ ? catalog_->effect(effect_guid) : nullptr;
    if (!d) return false;
    advance(now);
    AbilityChange c;
    apply_effect(c, *d, stacks, std::nullopt, now_, true);
    recompute(c);
    emit({std::move(c), std::nullopt, now_, "the effect " + d->row.name + " is given"});
    return true;
}
// (C Y B | C.O.A)

bool AbilityPipeline::remove_effect(int64_t effect_guid, double now) {
    advance(now);
    AbilityChange c;
    bool any = false;
    for (size_t i = 0; i < effects_.size();) {
        if (effects_[i].def->row.guid == effect_guid) {
            remove_instance(c, i);
            any = true;
        } else {
            ++i;
        }
    }
    if (!any) return false;
    recompute(c);
    emit({std::move(c), std::nullopt, now_, "an effect is taken off"});
    return true;
}

void AbilityPipeline::set_moving(bool moving) {
    moving_ = moving;
    stats_.set_moving(moving);
}

double AbilityPipeline::progress_of(const std::string& group) const {
    if (group == "SongGroup") {
        // the running loop of the montage group (a song of the SongGroup): how far its effects have got, 0 to 1
        for (const EffectInst& e : effects_) {
            if (e.owner != 0 && std::isinf(e.end)) return std::clamp((clock_ - e.start) / kSongProgressSeconds, 0.0, 1.0);
        }
        return 0;
    }
    if (group.empty()) return 1.0;   // the form with no argument: the ability whose hit this is has played [modelled]
    if (group == "DefaultGroup" && last_item_at_ >= 0 && item_instance_) return std::clamp((clock_ - last_item_at_) / kCastSeconds, 0.0, 0.99);
    return 0;
}

double AbilityPipeline::remaining_charges(int64_t ability) const {
    const AbilityDef* d = catalog_ ? catalog_->ability(ability) : nullptr;
    if (!d) return 0;
    const auto g = groups_.find(group_key(d->row));
    if (g != groups_.end()) return g->second.charges;
    const auto m = last_max_charges_.find(ability);
    return m == last_max_charges_.end() ? 1.0 : m->second;
}

double AbilityPipeline::stat_delta_over_time(int64_t stat, double seconds) const {
    if (stat != content::kGuidHealth) return 0;
    double sum = 0;
    for (const auto& [at, lost] : losses_) {
        if (clock_ - at <= seconds) sum -= lost;   // a loss is a negative change
    }
    return sum;
}

void AbilityPipeline::sync_a7_values(double now) {
    drop_evaluation();
    const content::AbilityRules& rules = a7_.rules();
    if (catalog_) {
        if (const EffectDef* sprint = catalog_->effect(rules.sprint.effect_guid)) {
            for (const content::ModRow& m : sprint->mods) {
                if (m.value_expr == 0 || m.shield_absorb) continue;
                double v = 0;
                std::string why;
                if (eval(m.value_expr, v, why)) a7_.set_effect_value(rules.sprint.effect_guid, m.stat_guid, v, now);
            }
        }
    }
    if (const stats::StatValue* regen = stats_.find(content::kGuidStaminaRegeneration)) {
        if (regen->value >= 0 && std::isfinite(regen->value)) a7_.set_regen_rate(regen->value, now);
    }
}

void AbilityPipeline::refresh(double now, const char* why, bool with_stats) {
    advance(now);
    AbilityChange c;
    if (stats_.class_enum() != resource_class_) {
        // the class changed: the old class's effects (its resource, its buffs) leave with it, and the new class's resource is the rule's
        while (!effects_.empty()) remove_instance(c, effects_.size() - 1);
        resolve_resource();
    }
    if (with_stats) {
        recompute(c);
    } else {
        drop_evaluation();
        if (stats_.started()) stats_.set_effects(engine_effects());
    }
    emit({std::move(c), std::nullopt, now_, why});
}

// ---- effects --------------------------------------------------------------------------------------------------------------------------
// CoA / C.Y.B

int AbilityPipeline::stacks_of(int64_t effect_guid) const {
    int n = 0;
    for (const EffectInst& e : effects_) {
        if (e.def->row.guid == effect_guid) n += e.stacks;
    }
    return n;
}

std::optional<double> AbilityPipeline::effect_end(int64_t effect_guid) const {
    for (const EffectInst& e : effects_) {
        if (e.def->row.guid == effect_guid) return e.end;
    }
    return std::nullopt;
}

bootstrap::EffectItem AbilityPipeline::effect_item(const EffectInst& e) const {
    bootstrap::EffectItem item;
    item.effect_id = e.key;
    item.effect_guid = e.def->row.guid;
    item.start_time = sync(e.start);
    item.duration = std::isinf(e.end) ? AbilityRuntime::kInfiniteDuration : static_cast<float>(e.end - e.start);
    item.tick_time = static_cast<float>(e.def->row.tick_timer);
    item.stacks = e.stacks;
    return item;
}

void AbilityPipeline::apply_effect(AbilityChange& c, const EffectDef& def, int stacks_in, std::optional<double> duration, double t, bool raw, int64_t owner) {
    const content::EffectRow& row = def.row;
    if (stacks_in < 1) stacks_in = 1;
    // the duration: the hit row's, the record's (a number of seconds), or none: infinite
    double dur = kInf;
    if (duration) {
        dur = *duration;
    } else if (!row.infinite_duration && row.duration_expr != 0) {
        std::string why;
        if (!eval(row.duration_expr, dur, why)) {
            AbilityEvent e;
            e.at = t;
            e.note = "the effect " + row.name + " is not applied: its duration does not run (" + why + ")";
            e.refusal = false;
            events_.push_back(std::move(e));
            return;
        }
        if (!(dur >= 0)) dur = 0;
    }
    // the values of its stat mods, for one stack, as the character is now
    std::vector<stats::EffectMod> mods;
    std::optional<float> shield;   // a mod on TempHealth is a shield: its amount is held by the instance, not run through the engine
    for (const content::ModRow& m : def.mods) {
        if (m.shield_absorb || m.value_expr == 0) continue;
        double v = 0;
        std::string why;
        if (!eval(m.value_expr, v, why)) continue;   // a mod that does not run is left out (the engine's none)
        if (m.stat_guid == content::kGuidTempHealth) {
            if (std::isfinite(v) && v > 0) shield = static_cast<float>(shield.value_or(0.0f) + v);
            continue;
        }
        mods.push_back({m.stat_guid, v});
    }
    auto give_shield = [&](EffectInst& e) {
        if (!shield) return;
        e.has_shield = true;
        e.shield_base = e.shield_value = *shield;
    };
    const int in_place_limit = [&] {
        if (row.in_place_stack_limit_expr != 0) {
            double v = 0;
            std::string why;
            if (eval(row.in_place_stack_limit_expr, v, why) && v >= 1) return static_cast<int>(std::min(v, 4096.0));
        }
        return std::max(1, row.global_stack_limit);
    }();
    const bool never_refresh = row.stack_duration_refresh.find("NeverRefresh") != std::string::npos;
    const bool extend = row.stack_duration_refresh.find("ExtendDuration") != std::string::npos;
    const bool in_place = row.stack_type.find("StackInPlace") != std::string::npos;
    const bool separate = row.stack_type.find("StackSeperate") != std::string::npos;
    const bool override_duration = row.stack_type.find("OverrideDuration") != std::string::npos;
// Cyb + C O A

    EffectInst* existing = nullptr;
    int same = 0;
    for (EffectInst& e : effects_) {
        if (e.def != &def) continue;
        ++same;
        if (!existing) existing = &e;
    }
    auto refreshed = [&](EffectInst& e) {   // the refresh rule of the record on an instance that is applied again
        if (never_refresh) return;
        give_shield(e);   // a shield that is applied again is given in full again
        double total = dur;
        if (extend && !std::isinf(e.end) && !std::isinf(dur)) total = (e.end - t) + dur;   // the remaining time plus the full one
        e.start = t;
        e.end = std::isinf(total) ? kInf : t + total;
    };
    auto fresh = [&](int stacks) {
        EffectInst e;
        e.key = a7_.allocate_effect_id();
        e.def = &def;
        e.owner = owner;
        e.zone = zone_owner_;
        e.stacks = stacks;
        e.start = t;
        e.end = std::isinf(dur) ? kInf : t + dur;
        e.mods = mods;
        e.next_tick = row.tick_timer > 0 ? (row.ignore_initial_tick ? t + row.tick_timer : t) : kInf;
        give_shield(e);
        for (uint32_t& w : e.shield_guid) w = static_cast<uint32_t>(guid_rng_.next() >> 16);
        effects_.push_back(e);
        c.effects_added.push_back(effect_item(effects_.back()));
        // the effect's Apply hits run now that it is on [R: Rogue_Stealth's effect confirms its Apply hit with the effect's item]
        if (apply_depth_ < kMaxApplyDepth) {
            ++apply_depth_;
            const EffectInst created = effects_.back();
            sync_engine_effects();
            run_effect_hits(c, created, kApplyPhases, 1, t);
            --apply_depth_;
        }
    };

    if (!existing) {
        if (effects_.size() >= kMaxEffects) return;
        fresh(raw ? std::min(stacks_in, std::max(1, row.global_stack_limit)) : in_place ? std::min(stacks_in, in_place_limit) : 1);
        return;
    }
    if (in_place) {
        existing->stacks = std::min(in_place_limit, existing->stacks + stacks_in);
        existing->mods = mods;
        refreshed(*existing);
        c.effects_changed.push_back(effect_item(*existing));
    } else if (separate) {
        if (same < std::max(1, row.global_stack_limit) && effects_.size() < kMaxEffects) fresh(1);
    } else if (override_duration) {
        existing->mods = mods;
        refreshed(*existing);
        c.effects_changed.push_back(effect_item(*existing));
    } else {   // OverrideAlways: one instance, replaced (a never-refreshing one keeps what it has)
        if (never_refresh) return;
        const int64_t old_key = existing->key;
        for (size_t i = 0; i < effects_.size(); ++i) {
            if (effects_[i].key == old_key) {
                remove_instance(c, i);
                break;
            }
        }
        fresh(1);
    }
}

void AbilityPipeline::remove_instance(AbilityChange& c, size_t index) {
    const int64_t key = effects_[index].key;
    // an instance the same change added never reached the client: it is dropped from the change, not removed
    bool unsent = false;
    for (auto it = c.effects_added.begin(); it != c.effects_added.end(); ++it) {
        if (it->effect_id == key) {
            c.effects_added.erase(it);
            unsent = true;
            break;
        }
    }
    for (auto it = c.effects_changed.begin(); it != c.effects_changed.end();) {
        it = it->effect_id == key ? c.effects_changed.erase(it) : it + 1;
    }
    if (!unsent) c.effects_removed.push_back(key);
    effects_.erase(effects_.begin() + static_cast<std::ptrdiff_t>(index));
}

void AbilityPipeline::remove_stacks(AbilityChange& c, int64_t effect_guid, int stacks) {
    for (size_t i = 0; i < effects_.size() && stacks > 0;) {
        EffectInst& e = effects_[i];
        if (e.def->row.guid != effect_guid) {
            ++i;
            continue;
        }
        const int take = std::min(stacks, e.stacks);
        e.stacks -= take;
        stacks -= take;
        if (e.stacks <= 0) {
            remove_instance(c, i);
        } else {
            c.effects_changed.push_back(effect_item(e));
            ++i;
        }
    }
}

void AbilityPipeline::sync_engine_effects() {
    drop_evaluation();
    if (stats_.started()) stats_.set_effects(engine_effects());
}

void AbilityPipeline::remove_by_hit(AbilityChange& c, const HitDef& hit) {
    int left = hit.max_remove > 0 ? hit.max_remove : 1 << 20;
    for (const int64_t guid : hit.remove_effects) {
        for (size_t i = 0; i < effects_.size() && left > 0;) {
            if (effects_[i].def->row.guid != guid) {
                ++i;
                continue;
            }
            remove_instance(c, i);
            --left;
        }
    }
    sync_engine_effects();
}
// C:Y:B with C-O-A

void AbilityPipeline::run_effect_hits(AbilityChange& c, const EffectInst& e, const char* const* phases, size_t n, double t) {
    HitSource src;   // an effect's hit: confirmed by the server when it lands, Source is the pawn [R: 1,525 effect hits]
    src.type = content::kSourceTypeEffect;
    src.guid = e.def->row.guid;
    src.actor = true;
    src.confirm = true;
    // a tick's hit is predicted on both clients, an Apply, Expire or End hit on neither [R: 784 tick hits (1,1), 541 Apply hits (0,0), 8 End and Expire]
    const bool tick = n == 1 && std::string_view(phases[0]) == "Tick";
    src.simulated = tick;
    src.autonomous = tick;
    for (const auto& [phase, hit_guid] : e.def->hits) {
        bool wanted = false;
        for (size_t i = 0; i < n; ++i) wanted = wanted || phase == phases[i];
        if (!wanted) continue;
        if (const HitDef* h = catalog_->hit(hit_guid)) apply_hit(c, *h, t, false, src);
    }
}

// ---- shields and damage -----------------------------------------------------------------------------------------------------------------

bool AbilityPipeline::absorb(AbilityChange&, double& amount) {
    bool any = false;
    for (EffectInst& e : effects_) {   // oldest first (the order the instances were made)
        if (!e.has_shield || !(e.shield_value > 0) || !(amount > 0)) continue;
        const float take = static_cast<float>(std::min(static_cast<double>(e.shield_value), amount));
        e.shield_value -= take;
        amount -= take;
        any = true;
    }
    return any;
}

void AbilityPipeline::sync_shields(AbilityChange& c) {
    std::vector<bootstrap::ShieldElement> now;
    for (const EffectInst& e : effects_) {
        if (!e.has_shield) continue;
        bootstrap::ShieldElement el;
        for (int i = 0; i < 4; ++i) el.guid[i] = e.shield_guid[i];
        el.base = e.shield_base;
        el.value = e.shield_value;
        now.push_back(el);
    }
    if (now == shields_sent_) return;
    shields_sent_ = now;
    c.shields = std::move(now);
}

std::vector<std::pair<double, double>> AbilityPipeline::shields() const {
    std::vector<std::pair<double, double>> out;
    for (const EffectInst& e : effects_) {
        if (e.has_shield) out.emplace_back(e.shield_base, e.shield_value);
    }
    return out;
}

double AbilityPipeline::apply_delta(AbilityChange& c, int64_t stat, double amount) {
    if (!std::isfinite(amount)) return 0;
    if (stat == content::kGuidStamina) return a7_.add_stamina(c, amount);
    if (stat != content::kGuidHealth && stat != content::kGuidMana) {
        // a class resource (a Ranger's Focus, a Cleric's DivinePower ...): held between its limits
        const stats::StatValue* v = stats_.find(stat);
        if (!v || !v->resource()) return 0;
        const double cur = v->value;
        double next = cur + amount;
        if (v->has_max) next = std::min(next, static_cast<double>(v->max));
        if (v->has_min) next = std::max(next, static_cast<double>(v->min));
        if (static_cast<float>(next) != static_cast<float>(cur)) set_held(c, stat, next);
        return next - cur;
    }
    const int64_t max_stat = stat == content::kGuidHealth ? content::kGuidMaxHealth : content::kGuidMaxMana;
    const double max = std::max(0.0, stat_now(max_stat));
    const double cur = stat_now(stat);
    double through = amount;
    if (stat == content::kGuidHealth && amount < 0) {   // a loss reaches Health through the shields
        double loss = -amount;
        absorb(c, loss);
        through = -loss;
    }
    const double next = std::clamp(cur + through, 0.0, max);
    if (static_cast<float>(next) != static_cast<float>(cur)) set_held(c, stat, next);
    if (stat == content::kGuidHealth && next < cur) {
        losses_.emplace_back(clock_, cur - next);
        while (!losses_.empty() && clock_ - losses_.front().first > 60.0) losses_.pop_front();
        if (losses_.size() > 256) losses_.pop_front();
    }
    return next - cur;
}

double AbilityPipeline::take_damage(double amount, double now) {
    advance(now);
    AbilityChange c;
    const double before = stat_now(content::kGuidHealth);
    apply_delta(c, content::kGuidHealth, -std::max(0.0, amount));
    recompute(c);
    const double through = before - stat_now(content::kGuidHealth);
    emit({std::move(c), std::nullopt, now_, "damage reaches the character"});
    return through;
}

void AbilityPipeline::apply_hit_on_targets(AbilityChange& c, const HitDef& hit, const std::vector<uint64_t>& targets, double t, const HitSource& src,
                                           const AbilityDef* ability) {
    if (!combat_ || targets.empty()) return;
    std::string why;
    bool ok = true;
    if (!eval_bool(hit.row.requirement_expr, true, ok, why) || !ok) return;
    if (!ensure_evaluation(why)) return;
    const int mask = hit.row.applicable_targets;
    double max_targets = 40;
    if (hit.row.max_targets_expr != 0) {
        double v = 0;
        if (eval(hit.row.max_targets_expr, v, why) && v >= 1) max_targets = std::min(max_targets, v);
    }
    const bool melee = ability && (ability->row.name.rfind("WeaponCombo_Melee", 0) == 0 ||
                                   std::find(ability->row.tags.begin(), ability->row.tags.end(), "ability.Melee") != ability->row.tags.end());
    std::vector<bootstrap::HitTargetOut> outs;
    for (const uint64_t id : targets) {
        if (static_cast<double>(outs.size()) >= max_targets) break;
        const combat::TargetInfo ti = combat_->inspect(id);
        if (!ti.exists || !ti.alive) continue;
        const bool hostile = ti.relation == combat::Relation::Hostile;
        if (hostile ? (mask & 2) == 0 : (mask & (1 | 4 | 8 | 16)) == 0) continue;   // a side this hit does not reach
        if (ti.surface_distance_cm > combat_->reach_cm(melee) + combat_->data().activation_leeway_cm()) continue;
        combat::Fighter* tf = combat_->fighter(id);
        if (!tf) continue;
        std::string err;
        auto target_eval = tf->stats().evaluation(tf->worn(), err, true);
        if (!target_eval) continue;
        Host host(*this);
        combat::HitMeta meta;
        meta.source_type = src.type;
        meta.source_guid = src.guid;
        if (ability) meta.ability_tags = ability->row.tags;
        meta.targets = static_cast<double>(targets.size());
        combat::Resolved r;
        if (!combat_->resolver().resolve_with(*evaluation_, host, target_eval.get(), ti.surface_distance_cm, hostile, ti.relation == combat::Relation::Neutral, hit, meta, r, why)) {
            if (no_formula_.insert(hit.row.guid).second) spdlog::warn("[abilities] the hit {} does not run on another actor: {}", hit.row.name, why);
            continue;
        }
        const combat::Delivery d = combat_->deliver(id, r, t);
        if (!d.applied) continue;
        bootstrap::HitTargetOut out;
        out.component = combat_->guid_of(id);          // the HitComp is the target actor [R: research/hitfx 1]
        out.cylinder = combat_->hit_component_of(id);  // exported with the confirmation when the client has no GUID for it yet
        // the impact is at the target [R: median 0 cm from its replicated position]; the normal a unit vector, here from the target towards the caster
        // [modelled: retail's normals vary; a unit vector is all the captures fix]
        out.has_impact = true;
        out.impact_location = {ti.position.x, ti.position.y, ti.position.z};
        {
            const combat::Vec3 from = combat_->self().position();
            double nx = from.x - ti.position.x, ny = from.y - ti.position.y, nz = from.z - ti.position.z;
            const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6) { nx /= len; ny /= len; nz /= len; } else { nx = 0; ny = 0; nz = 1; }
            out.impact_normal = {nx, ny, nz};
        }
        out.flags = d.flags;
        for (size_t i = 0; i < r.mods.size(); ++i) {
            // a stat mod whose value is 0 is not on the wire [R: the book's Weapon_Book_Hit_n, whose mod is the data's "0", carries no StatMod in
            // 408 of 408 retail confirmations; a heal at full Health keeps its Value with EffectiveValue 0]
            if (r.mods[i].value == 0.0 && d.effective[i] == 0.0) continue;
            out.mods.push_back({r.mods[i].stat_mod_guid, static_cast<float>(r.mods[i].value), static_cast<float>(d.effective[i])});
        }
        outs.push_back(std::move(out));
        // what the hit applies to its target: a shield goes to the target (an effect's other mods are the NPC world's to take, part 2)
        if (!r.miss()) {
            for (const content::HitEffectRow& row : hit.effects) {
                if (row.kind == "self") continue;
                const EffectDef* def = catalog_->effect(row.effect_guid);
                if (!def) continue;
                bool applies = true;
                if (!eval_bool(row.requirement_expr, true, applies, why) || !applies) continue;
                for (const content::ModRow& m : def->mods) {
                    if (m.stat_guid != content::kGuidTempHealth || m.value_expr == 0) continue;
                    double amount = 0;
                    if (!eval(m.value_expr, amount, why) || !(amount > 0)) continue;
                    double seconds = 0;
                    if (!def->row.infinite_duration && def->row.duration_expr != 0) eval(def->row.duration_expr, seconds, why);
                    if (row.duration_expr != 0) eval(row.duration_expr, seconds, why);
                    combat_->shield(id, amount, std::max(0.0, seconds), t);
                }
            }
        }
    }
    // what the hit applies to its caster ("self" rows) comes with the hit once, if it found anyone
    if (!outs.empty()) {
        for (const content::HitEffectRow& row : hit.effects) {
            if (row.kind != "self") continue;
            bool applies = true;
            if (!eval_bool(row.requirement_expr, true, applies, why) || !applies) continue;
            const EffectDef* def = catalog_->effect(row.effect_guid);
            if (!def) continue;
            double stacks = 1;
            if (row.stack_count_expr != 0 && !eval(row.stack_count_expr, stacks, why)) stacks = 1;
            std::optional<double> duration;
            if (row.duration_expr != 0) {
                double dsec = 0;
                if (eval(row.duration_expr, dsec, why)) duration = std::max(0.0, dsec);
            }
            apply_effect(c, *def, static_cast<int>(std::clamp(stacks, 1.0, 4096.0)), duration, t);
            sync_engine_effects();
        }
    }
    drop_evaluation();
    if (outs.empty()) return;
    const auto confirm = [&](std::vector<bootstrap::HitTargetOut> targets, std::optional<protocol::emit::FIntrepidNetworkGUID> source) {
        bootstrap::HitConfirm h;
        h.record_guid = hit.row.guid;
        h.source_type = src.type;
        h.source_guid = src.guid;
        h.source_is_actor = src.actor;
        h.source = source;
        h.targets = std::move(targets);
        h.predicted_autonomous = src.autonomous;
        h.predicted_simulated = src.simulated;
        h.response = src.response;
        confirmations_.push_back(std::move(h));
    };
    if (!src.source_is_target) {
        confirm(std::move(outs), std::nullopt);
        return;
    }
    // E1 stage 3 fix 3: a hit fired where a projectile landed names that target actor as its Source (the origin the client spawns the next projectile
    // from), one confirmation per target [R: the book's Split hits, 1,289 of 1,289: Source = the target, one target each]
    for (bootstrap::HitTargetOut& o : outs) {
        std::vector<bootstrap::HitTargetOut> one;
        const protocol::emit::FIntrepidNetworkGUID actor = o.component;
        one.push_back(std::move(o));
        confirm(std::move(one), actor);
    }
}

void AbilityPipeline::receive_hit(const combat::IncomingHit& in, double now) {
    advance(now);
    AbilityChange c;
    bootstrap::HitConfirm h;
    h.record_guid = in.hit_guid;
    h.source_type = in.source_type;
    h.source_guid = in.source_guid;
    h.source_is_actor = true;
    h.predicted_autonomous = true;
    if (combat_) h.instigator = combat_->guid_of(in.instigator);
    uint8_t flags = in.flags;
    const double before = stat_now(content::kGuidHealth);
    if ((flags & stats::kHitMiss) == 0) {
        for (const combat::ResolvedMod& m : in.mods) {
            if (!std::isfinite(m.value)) continue;
            const double effective = apply_delta(c, m.stat_guid, m.value);
            h.mods.push_back({m.stat_mod_guid, static_cast<float>(m.value), static_cast<float>(effective)});
        }
    }
    if (before > 0 && stat_now(content::kGuidHealth) <= 0) flags |= stats::kHitLethal;   // (the hit took the last Health)
    h.flags = flags;
    confirmations_.push_back(std::move(h));
    drop_evaluation();
    recompute(c);
    emit({std::move(c), std::nullopt, now_, "a hit of actor " + std::to_string(in.instigator) + " on the character"});
}

void AbilityPipeline::confirm_hit_on(const combat::IncomingHit& in, double now, const protocol::emit::FIntrepidNetworkGUID& target,
                                     const protocol::emit::FIntrepidNetworkGUID& cylinder, double taken, bool lethal) {
    advance(now);
    bootstrap::HitConfirm h;
    h.record_guid = in.hit_guid;
    h.source_type = in.source_type;
    h.source_guid = in.source_guid;
    h.source_is_actor = true;
    h.predicted_autonomous = true;
    if (combat_) h.instigator = combat_->guid_of(in.instigator);
    bootstrap::HitTargetOut t;
    t.component = target;
    t.cylinder = cylinder;
    uint8_t flags = in.flags;
    bool health_done = false;
    if ((flags & stats::kHitMiss) == 0) {
        for (const combat::ResolvedMod& m : in.mods) {
            if (!std::isfinite(m.value)) continue;
            double effective = 0;
            if (m.stat_guid == content::kGuidHealth && m.value < 0 && !health_done) {
                effective = -taken;   // the part that took effect: what came off the mount's Health
                health_done = true;
            }
            t.mods.push_back({m.stat_mod_guid, static_cast<float>(m.value), static_cast<float>(effective)});
        }
    }
    if (lethal) flags |= stats::kHitLethal;
    t.flags = flags;
    h.targets.push_back(std::move(t));
    h.flags = flags;
    confirmations_.push_back(std::move(h));
    emit({AbilityChange{}, std::nullopt, now_, "a hit of actor " + std::to_string(in.instigator) + " on the character's mount"});
}
// -- C O A & Cyb --

void AbilityPipeline::apply_hit(AbilityChange& c, const HitDef& hit, double t, bool skip_resource, const HitSource& src) {
    if (!hits_caster(hit.row)) return;
    bool ok = true;
    std::string why;
    if (!eval_bool(hit.row.requirement_expr, true, ok, why) || !ok) return;
    if (!hit.remove_effects.empty()) remove_by_hit(c, hit);   // (what the hit takes off goes before what it applies)
    for (const content::HitEffectRow& row : hit.effects) {
        bool applies = true;
        if (!eval_bool(row.requirement_expr, true, applies, why) || !applies) continue;
        const EffectDef* def = catalog_->effect(row.effect_guid);
        if (!def) continue;
        if (skip_resource && resource_ && def == resource_) continue;   // the class rule gains the resource: a cast never gains two
        double stacks = 1;
        if (row.stack_count_expr != 0 && !eval(row.stack_count_expr, stacks, why)) stacks = 1;
        std::optional<double> duration;
        if (row.duration_expr != 0) {
            double d = 0;
            if (eval(row.duration_expr, d, why)) duration = std::max(0.0, d);
        }
        apply_effect(c, *def, static_cast<int>(std::clamp(stacks, 1.0, 4096.0)), duration, t);
        sync_engine_effects();   // the character's effects changed: the next value is read against them
    }
    // the hit's own stat mods: the formula's value moves the stat at once (a heal clamped at the maximum, a loss through the shields); the
    // confirmation carries the value and the part that took effect, which is where the client draws its floating number
    std::vector<bootstrap::HitStatModOut> outs;
    for (const content::ModRow& m : hit.mods) {
        if (m.shield_absorb || m.value_expr == 0) continue;
        double amount = 0;
        if (!eval(m.value_expr, amount, why) || !std::isfinite(amount)) continue;
        const double effective = apply_delta(c, m.stat_guid, amount);
        outs.push_back({m.stat_mod_guid, static_cast<float>(amount), static_cast<float>(effective)});
    }
    drop_evaluation();
    // [R: 6,955 requests of the retail captures] a hit the client asked for is confirmed when it has stat mods (the client draws their numbers), an
    // effect to show (OnHitFXId, OnActivateFXId) or Party in its mask: 664 of 674 with mods and an FX, 66 of 66 with an FX only, 11 of 11 with an
    // activation FX only, 6 of 74 with none of them; never a Default_Self hit
    if (src.confirm && (!outs.empty() || hit.row.has_fx || (hit.row.applicable_targets & 8) != 0)) {
        bootstrap::HitConfirm h;
        h.record_guid = hit.row.guid;
        h.source_type = src.type;
        h.source_guid = src.guid;
        h.source_is_actor = src.actor;
        h.mods = std::move(outs);
        h.predicted_autonomous = src.autonomous;
        h.predicted_simulated = src.simulated;
        h.response = src.response;
        confirmations_.push_back(std::move(h));
    }
}

void AbilityPipeline::effect_expires(size_t index, double t) {
    AbilityChange c;
    const EffectInst gone = effects_[index];
    remove_instance(c, index);
    run_effect_hits(c, gone, kExpirePhases, 2, t);
    recompute(c);
    emit({std::move(c), std::nullopt, t, "the effect " + gone.def->row.name + " ends"});
}

void AbilityPipeline::effect_ticks(size_t index, double t) {
    EffectInst& e = effects_[index];
    const double period = e.def->row.tick_timer;
    e.next_tick = period > 0 ? t + period : kInf;
    const EffectInst copy = e;
    AbilityChange c;
    // a looping ability's own effect re-adds the others every tick (a song's Buff lasts 2.5 s and is added every 2.0 s)
    if (copy.owner != 0 && copy.def->row.infinite_duration) {
        if (const AbilityDef* a = catalog_->ability(copy.owner)) {
            for (const auto& [key, guid] : a->effects) {
                if (guid == copy.def->row.guid) continue;
                if (const EffectDef* sibling = catalog_->effect(guid)) apply_effect(c, *sibling, 1, std::nullopt, t, false, copy.owner);
            }
            sync_engine_effects();
        }
    }
    run_effect_hits(c, copy, kTickPhases, 1, t);
    recompute(c);
    emit({std::move(c), std::nullopt, t, "the effect " + copy.def->row.name + " ticks"});
}

// ---- cooldowns ------------------------------------------------------------------------------------------------------------------------

bootstrap::CooldownItem AbilityPipeline::cooldown_item(int64_t ability, const Group& g) const {
    bootstrap::CooldownItem cd;
    cd.ability_guid = ability;
    cd.triggered = true;
    cd.initial_cooldown = static_cast<float>(g.cooldown);
    // retail: the start is the chain's first (a charge that returns keeps it), the end is the start plus the float, once for each timer of the chain
    cd.start_time = sync(g.chain_start);
    cd.end_time = cd.start_time + static_cast<double>(std::max(1, g.timers)) * static_cast<double>(cd.initial_cooldown);
    cd.charges = g.charges;
    return cd;
}

void AbilityPipeline::use_charge(AbilityChange& c, const Cast& cast, double t) {
    if (!(cast.triggers_cooldown && cast.cooldown > 0)) return;
    Group& g = groups_[cast.group];
    if (g.items.empty() && g.timer_end < 0) {
        g.charges = cast.max_charges;
        g.chain_start = t;
        g.timers = 0;
    }
    g.max_charges = cast.max_charges;
    g.charges = std::min(g.charges, g.max_charges);
    g.cooldown = cast.cooldown;
    g.charges = std::max(0, g.charges - cast.charge_cost);
    if (g.timer_end < 0) {
        g.timer_start = t;
        g.timer_end = t + g.cooldown;
        g.chain_start = t;   // (a group whose last item was dropped and whose charges are all back starts a new chain)
        g.timers = 1;
    }
    g.items.insert(cast.def->row.guid);
    g.last_ability = cast.def->row.guid;
    for (const int64_t a : g.items) c.cooldowns_set.push_back(cooldown_item(a, g));
}

void AbilityPipeline::group_timer_ends(const std::string& key, double t) {
    Group& g = groups_.at(key);
    AbilityChange c;
    g.charges = std::min(g.max_charges, g.charges + 1);
    if (g.charges < g.max_charges) {
        g.timer_start = g.timer_end;
        g.timer_end = g.timer_start + g.cooldown;
        ++g.timers;
        for (const int64_t a : g.items) c.cooldowns_set.push_back(cooldown_item(a, g));
        emit({std::move(c), std::nullopt, t, "a charge is back"});
        return;
    }
    for (const int64_t a : g.items) c.cooldowns_removed.push_back(a);
    groups_.erase(key);
    emit({std::move(c), std::nullopt, t, "the cooldown ends"});
}

int AbilityPipeline::charges_left(int64_t ability) const {
    if (!catalog_) return -1;
    const AbilityDef* def = catalog_->ability(ability);
    if (!def) return -1;
    const auto it = groups_.find(group_key(def->row));
    return it == groups_.end() ? -1 : it->second.charges;
}

// ---- movement ---------------------------------------------------------------------------------------------------------------------------

double AbilityPipeline::movement_allowance(double now) const {
    double speed = 0;
    for (const auto& [until, s] : movement_) {
        if (now <= until) speed = std::max(speed, s);
    }
    return speed;
}
// (C-O-A | C-Y-B)

// ---- regeneration -----------------------------------------------------------------------------------------------------------------------

void AbilityPipeline::set_dead(bool dead) {
    dead_ = dead;
    if (dead) casts_.clear();
}

void AbilityPipeline::regenerate(double t) {
    next_regen_ = t + kRegenInterval;
    if (dead_) return;
    AbilityChange c;
    struct Trio {
        int64_t current, regeneration, maximum;
    };
    for (const Trio tr : {Trio{content::kGuidHealth, content::kGuidHealthRegeneration, content::kGuidMaxHealth},
                          Trio{content::kGuidMana, content::kGuidManaRegeneration, content::kGuidMaxMana}}) {
        if (!stats_.find(tr.current) || !stats_.find(tr.regeneration) || !stats_.find(tr.maximum)) continue;
        const double cur = stat_now(tr.current), reg = stat_now(tr.regeneration), max = stat_now(tr.maximum);
        if (!(reg > 0) || !(cur < max)) continue;
        const float next = std::min(static_cast<float>(max), static_cast<float>(cur) + static_cast<float>(reg));
        if (!(next > static_cast<float>(cur))) continue;
        set_held(c, tr.current, next);
    }
    // the class resources: every second a resource with a regeneration program moves by it (a Ranger's Focus +3, a Cleric's DivinePower by its Gen,
    // a Fighter's Momentum down by what it holds), between its limits
    bool resource_moved = false;
    for (const stats::StatValue& v : stats_.values()) {
        if (!v.resource() || !v.has_regen || v.regen == 0.0f || !(v.replication == 1 || v.replication == 5 || v.replication == 7)) continue;
        double next = static_cast<double>(v.value) + static_cast<double>(v.regen) * kRegenInterval;
        if (v.has_max) next = std::min(next, static_cast<double>(v.max));
        if (v.has_min) next = std::max(next, static_cast<double>(v.min));
        if (static_cast<float>(next) == v.value) continue;
        set_held(c, v.guid, next);
        resource_moved = true;
    }
    if (resource_moved) recompute(c);   // (what a resource's regeneration reads of itself, and of the others, changed)
    if (c.empty()) return;
    ++regen_ticks_;
    emit({std::move(c), std::nullopt, t, "Health and Mana regenerate"});
}

// ---- time -------------------------------------------------------------------------------------------------------------------------------

double AbilityPipeline::next_due() const {
    double due = next_regen_;
    for (const Cast& c : casts_) {
        for (const std::optional<double>* d : {&c.item_due, &c.cost_due, &c.hit_due, &c.projectile_due, &c.loop_due, &c.own_due}) {
            if (*d) due = std::min(due, **d);
        }
    }
    for (const CastRecord& r : records_) {
        for (const HitSlot& s : r.slots) due = std::min(due, slot_due(r, s));
    }
    for (const Held& h : held_) {
        if (h.end_due) due = std::min(due, *h.end_due);
    }
    for (const Instance& i : instances_) {
        if (i.end_due) due = std::min(due, *i.end_due);
    }
    for (const EffectInst& e : effects_) due = std::min({due, e.end, e.next_tick});
    for (const auto& [key, g] : groups_) {
        if (g.timer_end >= 0) due = std::min(due, g.timer_end);
    }
    for (const Zone& z : zones_) due = std::min(due, zone_due(z));
    return due;
}

void AbilityPipeline::advance(double now) {
    if (now < now_) now = now_;   // the clock never goes back
    // a stall does not owe more than a few regeneration ticks
    if (now - next_regen_ > kMaxRegenSteps * kRegenInterval) next_regen_ = now - (kMaxRegenSteps - 1) * kRegenInterval;
    for (size_t guard = 0; guard < kMaxStepsPerAdvance; ++guard) {   // (every due thing moves its own time on: the bound is for data that does not)
        const double t = next_due();
        if (t > now) break;
        run_due(t);
    }
    now_ = now;
    prune_records(now);
}

void AbilityPipeline::tick(double now) { advance(now); }
// Cyb captures, C-O-A code

void AbilityPipeline::run_due(double t) {
    clock_ = t;
    for (size_t i = 0; i < casts_.size(); ++i) {
        Cast& cast = casts_[i];
        if (cast.item_due && *cast.item_due <= t) {
            cast.item_due.reset();
            step_item(cast, t);
            finish_cast_if_done(i);
            return;
        }
        if (cast.cost_due && *cast.cost_due <= t) {
            cast.cost_due.reset();
            step_cost(cast, t);
            finish_cast_if_done(i);
            return;
        }
        if (cast.hit_due && *cast.hit_due <= t) {
            cast.hit_due.reset();
            step_hits(cast, t, "hit");
            finish_cast_if_done(i);
            return;
        }
        if (cast.projectile_due && *cast.projectile_due <= t) {
            cast.projectile_due.reset();
            step_hits(cast, t, "projectile");
            finish_cast_if_done(i);
            return;
        }
        if (cast.loop_due && *cast.loop_due <= t) {
            cast.loop_due.reset();
            step_loop(cast, t);
            finish_cast_if_done(i);
            return;
        }
        if (cast.own_due && *cast.own_due <= t) {
            cast.own_due.reset();
            step_own(cast, t);
            finish_cast_if_done(i);
            return;
        }
    }
    for (size_t i = 0; i < held_.size(); ++i) {
        if (held_[i].end_due && *held_[i].end_due <= t) return end_held(i, t);
    }
    for (size_t i = 0; i < instances_.size(); ++i) {
        if (instances_[i].end_due && *instances_[i].end_due <= t) return end_instance(i, t);
    }
    for (CastRecord& rec : records_) {
        for (HitSlot& s : rec.slots) {
            for (size_t i = 0; i < s.due.size(); ++i) {
                if (s.due[i].at > t) continue;
                const Pending p = s.due[i];
                s.due.erase(s.due.begin() + static_cast<std::ptrdiff_t>(i));
                return run_slot(rec, s, t, true, p.found, p.targets);
            }
        }
    }
    for (size_t i = 0; i < effects_.size(); ++i) {
        if (effects_[i].end <= t) return effect_expires(i, t);
    }
    for (size_t i = 0; i < effects_.size(); ++i) {
        if (effects_[i].next_tick <= t) return effect_ticks(i, t);
    }
    for (auto& [key, g] : groups_) {
        if (g.timer_end >= 0 && g.timer_end <= t) {
            const std::string k = key;
            return group_timer_ends(k, t);
        }
    }
    for (size_t i = 0; i < zones_.size(); ++i) {
        if (zone_due(zones_[i]) <= t) return run_zone(i, t);
    }
    if (next_regen_ <= t) return regenerate(t);
}

void AbilityPipeline::finish_cast_if_done(size_t index) {
    const Cast& c = casts_[index];
    if (!c.item_due && !c.cost_due && !c.hit_due && !c.projectile_due && !c.loop_due && !c.own_due) casts_.erase(casts_.begin() + static_cast<std::ptrdiff_t>(index));
}

// ---- the cast ---------------------------------------------------------------------------------------------------------------------------

std::string AbilityPipeline::montage_group_of(const AbilityDef& def) const { return std::string(group_of_name(def.row.name)); }

// ---- the client's hit requests ------------------------------------------------------------------------------------------------------

const char* AbilityPipeline::verdict_name(HitVerdict v) {
    switch (v) {
        case HitVerdict::Scheduled: return "scheduled";
        case HitVerdict::Quiet: return "accepted quietly";
        case HitVerdict::Empty: return "found nobody";
        case HitVerdict::NoCast: return "no such cast";
        case HitVerdict::NotItsHit: return "not a hit of the ability";
        case HitVerdict::TooMany: return "too many requests";
        case HitVerdict::Unsupported: return "not served";
    }
    return "?";
}

int64_t AbilityPipeline::targeting_of(const AbilityDef& def, int key) const {
    for (const content::TargetingRow& t : def.targetings) {
        if (t.key == key) return t.guid;
    }
    return content::kTargetingDefaultSelf;   // a hit whose key names no targeting record is aimed at the caster
}
// ~ C | Y | B & C O A ~

std::set<int64_t> AbilityPipeline::reach_of(const AbilityDef& def) const {
    std::set<int64_t> out;
    std::vector<int64_t> todo;
    for (const content::AbilityHitRow& h : def.hits) todo.push_back(h.hit_guid);
    std::set<int64_t> effects_seen;
    auto add_effect = [&](int64_t guid) {
        if (!effects_seen.insert(guid).second) return;
        if (const EffectDef* e = catalog_->effect(guid)) {
            for (const auto& [phase, hit] : e->hits) todo.push_back(hit);
        }
    };
    for (const auto& [key, effect] : def.effects) add_effect(effect);
    while (!todo.empty() && out.size() < 512) {
        const int64_t g = todo.back();
        todo.pop_back();
        if (!out.insert(g).second) continue;
        if (const HitDef* h = catalog_->hit(g)) {
            for (const content::HitEffectRow& r : h->effects) add_effect(r.effect_guid);
        }
    }
    return out;
}

AbilityPipeline::CastRecord* AbilityPipeline::record_of(int32_t client_id) {
    for (CastRecord& r : records_) {
        if (r.client_id == client_id) return &r;
    }
    return nullptr;
}

void AbilityPipeline::drop_record(int32_t client_id) {
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->client_id == client_id) {
            records_.erase(it);
            return;
        }
    }
}

void AbilityPipeline::prune_records(double t) {
    for (auto it = records_.begin(); it != records_.end();) {
        bool pending = false;
        for (const HitSlot& s : it->slots) pending = pending || !s.due.empty();
        if (!pending && it->window_end < t) {
            it = records_.erase(it);
        } else {
            ++it;
        }
    }
}

double AbilityPipeline::slot_due(const CastRecord& rec, const HitSlot& s) const {
    double due = kInf;
    (void)rec;
    for (const Pending& p : s.due) due = std::min(due, p.at);
    return due;
}

AbilityPipeline::HitVerdict AbilityPipeline::hit_request(int64_t ability, int64_t targeting_guid, int64_t hit_guid, size_t components, size_t own, double now,
                                                         const std::vector<uint64_t>& targets) {
    advance(now);
    CastRecord* rec = nullptr;
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (it->ability == ability && now <= it->window_end) {
            rec = &*it;
            break;
        }
    }
    if (!rec) return HitVerdict::NoCast;
    if (!rec->reach.count(hit_guid)) return HitVerdict::NotItsHit;
    const int max_requests = rec->max_requests;
    HitSlot* slot = nullptr;
    for (HitSlot& s : rec->slots) {
        if (s.hit->row.guid == hit_guid) slot = &s;
    }
    if (!slot) {
        // a Default_Self hit (applied with the cast, never confirmed), or a hit of a projectile, a zone or an effect, which the server runs by itself
        if (targeting_guid == content::kTargetingDefaultSelf) return HitVerdict::Quiet;
        const auto [it, fresh] = rec->asked.try_emplace(hit_guid, std::make_pair(0, -1e9));
        (void)fresh;
        if (it->second.first >= max_requests || now - it->second.second < kMinRequestGap) return HitVerdict::TooMany;
        ++it->second.first;
        it->second.second = now;
        return HitVerdict::Unsupported;
    }
    // (the targeting record the client names is not checked against the ability's key map: 122 of 1,854 retail requests name another record than the
    // map's, and the hit is what decides what happens)
    if (slot->requests >= max_requests || now - slot->last_request < kMinRequestGap) return HitVerdict::TooMany;
    ++slot->requests;
    slot->last_request = now;
    const bool found = own > 0 && slot->reaches;
    (void)components;
    // the other actors the request names, for a hit that aims at others (a forged crowd is cut at the most a hit has targets)
    std::vector<uint64_t> others;
    if (slot->aims_others) {
        for (const uint64_t id : targets) {
            if (others.size() >= 40) break;
            if (std::find(others.begin(), others.end(), id) == others.end()) others.push_back(id);
        }
    }
    // the answer comes kConfirmDelay after the request, and never before the cast's own cost step (+ the hits' delay)
    const double at = std::max(now + kConfirmDelay, record_reference(*rec) + rec->hit_delay);
    slot->due.push_back({at, found, others});
    return (found || !others.empty()) ? HitVerdict::Scheduled : HitVerdict::Empty;
}

void AbilityPipeline::run_slot(CastRecord& rec, HitSlot& slot, double t, bool response, bool found, const std::vector<uint64_t>& targets) {
    AbilityChange c;
    const AbilityDef* def = catalog_->ability(rec.ability);
    HitSource src;
    src.type = content::kSourceTypeAbility;
    src.guid = rec.ability;
    src.actor = true;
    src.confirm = true;
    src.response = response;
    if (!targets.empty()) {
        apply_hit_on_targets(c, *slot.hit, targets, t, src, def);
        if (!found) {
            recompute(c);
            emit({std::move(c), std::nullopt, t,
                  "the hit " + slot.hit->row.name + " of " + (def ? def->row.name : std::string("?")) + " on " + std::to_string(targets.size()) + " other actor(s) (the client asked)"});
            return;
        }
    }
    if (!found) {
        // an area hit that found nobody: its confirmation has no target (when the hit is one the server confirms at all)
        bootstrap::HitConfirm h;
        h.record_guid = slot.hit->row.guid;
        h.source_type = src.type;
        h.source_guid = src.guid;
        h.no_target = true;
        h.predicted_autonomous = true;
        h.response = response;
        if (!slot.hit->mods.empty() || slot.hit->row.has_fx || (slot.hit->row.applicable_targets & 8) != 0) confirmations_.push_back(std::move(h));
    } else {
        apply_hit(c, *slot.hit, t, true, src);
    }
    recompute(c);
    emit({std::move(c), std::nullopt, t,
          "the hit " + slot.hit->row.name + " of " + (def ? def->row.name : std::string("?")) + (response ? " (the client asked)" : " (the server's own: no request came)") +
              (found ? "" : ": it found nobody")});
}

bool AbilityPipeline::cancel(int32_t client_instance_id, double now) {
    advance(now);
    // a held ability (Guard, Reflect) ends with the key's release: its effects go kHeldEndDelay later
    for (Held& h : held_) {
        if (h.client_id == client_instance_id && !h.end_due) h.end_due = now_ + kHeldEndDelay;
    }
    return std::find(recent_ids_.begin(), recent_ids_.end(), client_instance_id) != recent_ids_.end();
}

bool AbilityPipeline::end_request(int32_t server_instance_id, double now) {
    advance(now);
    for (Instance& i : instances_) {
        if (i.server_id != server_instance_id || i.end_due) continue;
        i.end_due = now_ + kEndDelay;
        return true;
    }
    return false;
}

void AbilityPipeline::end_instance(size_t index, double t) {
    const Instance inst = instances_[index];
    instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(index));
    AbilityEvent e;
    e.at = t;
    // the item (when it is still the ability item the pawn lists) keeps its place with the montage cleared, and the client is told the instance ended
    if (item_instance_ && *item_instance_ == inst.server_id) {
        if (const AbilityDef* a = catalog_ ? catalog_->ability(inst.ability) : nullptr) {
            bootstrap::ActiveAbilityItem item;
            item.ability_guid = inst.ability;
            item.server_instance_id = inst.server_id;
            item.montage_group = montage_group_of(*a);
            item.animation_counter = animation_counter_;
            e.change.abilities_changed.push_back(std::move(item));
        }
    }
    e.end_ability = inst.client_id;
    e.note = "the client ended the ability instance " + std::to_string(inst.server_id);
    events_.push_back(std::move(e));
}
// C O A and C Y B

std::set<int64_t> AbilityPipeline::effects_applied_by_hits(const AbilityDef& def) const {
    std::set<int64_t> out;
    std::set<int64_t> hits, effs_seen;
    std::vector<int64_t> todo;
    for (const content::AbilityHitRow& h : def.hits) todo.push_back(h.hit_guid);
    for (const auto& [key, effect] : def.effects) {
        if (const EffectDef* e = catalog_->effect(effect)) {
            for (const auto& [phase, hit] : e->hits) todo.push_back(hit);
        }
    }
    while (!todo.empty() && hits.size() < 512) {
        const int64_t g = todo.back();
        todo.pop_back();
        if (!hits.insert(g).second) continue;
        const HitDef* h = catalog_->hit(g);
        if (!h) continue;
        for (const content::HitEffectRow& r : h->effects) {
            out.insert(r.effect_guid);
            if (effs_seen.insert(r.effect_guid).second) {
                if (const EffectDef* e = catalog_->effect(r.effect_guid)) {
                    for (const auto& [phase, hit] : e->hits) todo.push_back(hit);
                }
            }
        }
    }
    return out;
}

const char* AbilityPipeline::resolve_formulas(Cast& cast, const AbilityDef& def, std::string& why) {
    const content::AbilityRow& row = def.row;
    if (row.cooldown_expr != 0 && !eval(row.cooldown_expr, cast.cooldown, why)) return "its cooldown";
    cast.cooldown = std::max(0.0, cast.cooldown);
    if (row.charges_expr != 0) {
        double v = 1;
        if (!eval(row.charges_expr, v, why)) return "its charges";
        cast.max_charges = static_cast<int>(std::clamp(std::round(v), 1.0, 99.0));
    }
    for (size_t i = 0; i < def.costs.size();) {
        const int ord = def.costs[i].cost_ord;
        const content::CostRow& first = def.costs[i];
        bool enabled = true;
        if (!eval_bool(first.enabled_expr, true, enabled, why)) return "a cost's enabled flag";
        int charge = 1;
        if (first.charge_cost_expr != 0) {
            double v = 1;
            if (!eval(first.charge_cost_expr, v, why)) return "a cost's charge count";
            charge = static_cast<int>(std::clamp(std::round(v), 0.0, 99.0));
        }
        size_t j = i;
        for (; j < def.costs.size() && def.costs[j].cost_ord == ord; ++j) {
            const content::CostRow& cr = def.costs[j];
            if (!enabled || cr.stat_ord < 0 || cr.stat_guid == 0) continue;
            double amount = 0;
            if (cr.amount_expr != 0 && !eval(cr.amount_expr, amount, why)) return "a cost's amount";
            if (amount > 0) cast.stat_costs.emplace_back(cr.stat_guid, amount);
        }
        if (enabled) {
            cast.triggers_cooldown = cast.triggers_cooldown || first.triggers_cooldown;
            if (first.triggers_cooldown) cast.charge_cost = std::max(cast.charge_cost, charge);
        }
        for (const content::EffectCostRow& ec : def.effect_costs) {
            if (ec.cost_ord != ord || !enabled || ec.effect_guid == 0) continue;
            double n = 0;
            if (ec.num_expr != 0 && !eval(ec.num_expr, n, why)) return "an effect cost's count";
            if (n >= 1) cast.effect_costs.emplace_back(ec.effect_guid, static_cast<int>(std::min(n, 4096.0)));
        }
        i = j;
    }
    return nullptr;
}

std::optional<AbilityPipeline::Inspection> AbilityPipeline::inspect(int64_t ability) {
    const AbilityDef* def = catalog_ ? catalog_->ability(ability) : nullptr;
    if (!def) return std::nullopt;
    Inspection out;
    Cast cast;
    cast.def = def;
    drop_evaluation();   // the character as it is now
    std::string why;
    if (const char* what = resolve_formulas(cast, *def, why)) out.problem = std::string(what) + ": " + why;
    out.cooldown = cast.cooldown;
    out.max_charges = cast.max_charges;
    out.charge_cost = cast.charge_cost;
    out.triggers_cooldown = cast.triggers_cooldown;
    out.group = group_key(def->row);
    for (const auto& [stat, amount] : cast.stat_costs) {
        std::string name;
        for (const content::CostRow& cr : def->costs)
            if (cr.stat_guid == stat) name = cr.stat_name;
        out.stat_costs.push_back({stat, name, amount});
    }
    out.effect_costs = cast.effect_costs;
    if (def->row.cast_expr != 0) {
        double v = 0;
        if (eval(def->row.cast_expr, v, why)) out.cast_seconds = v;
        else if (out.problem.empty()) out.problem = "its cast time: " + why;
    }
    if (!eval_bool(def->row.requirement_expr, true, out.requirement, why) && out.problem.empty()) out.problem = "its activation requirement: " + why;
    return out;
}
// [C | O | A - Cyb]

Refusal AbilityPipeline::trigger(int64_t ability, int32_t client_id, double now, const TriggerInfo& info) {
    advance(now);
    const double t = now_;
    // a ClientInstanceId the pipeline has seen lately is a replay (or a stale prediction key): dropped, nothing answered (ClientEndAbility would end the
    // real instance of that id)
    if (std::find(recent_ids_.begin(), recent_ids_.end(), client_id) != recent_ids_.end()) return Refusal::Duplicate;
    recent_ids_.push_back(client_id);
    if (recent_ids_.size() > kRecentIds) recent_ids_.pop_front();

    const AbilityDef* def = catalog_ ? catalog_->ability(ability) : nullptr;
    const std::string name = def ? def->row.name : std::to_string(ability);
    auto refused = [&](Refusal why, const std::string& extra = {}) {
        refuse(client_id, why, t, name + extra);
        return why;
    };
    if (dead_) return refused(Refusal::Dead);
    if (!def || a7_.enabled(ability)) return refused(Refusal::NotEnabled);
    if (!known(ability)) return refused(Refusal::NotKnown);
    if (info.item) return refused(Refusal::Unsupported, " (an item's ability is not run here)");
    // a trigger during another's cast is taken (retail's weapon combos and channels re-trigger within a montage; no refusal is captured), up to a few
    // waiting at once: more is a client that floods
    if (casts_.size() >= kMaxCasts) return refused(Refusal::Busy);
    const content::AbilityRow& row = def->row;

    // the shape
    // An ability that needs a target and can be aimed at a friend is refused without one (the client names the caster for an ally-on-self cast); one that
    // can only be aimed at enemies is cast at the air when there is none: it spends its cost, starts its cooldown, plays its montage and hits nobody
    // [R: Tank_Slam, ShieldAssault, Desolate, Mock]. Held and toggle abilities (Smart, Toggle) are plain casts here: the client's cancel is taken quietly.
    const bool needs_target = row.target_requirement.find("Always") != std::string::npos;
    constexpr int kFriendly = 1 | 4 | 8 | 16;   // Self, Ally, Party, Raid
    if (needs_target && info.target_count == 0 && (row.applicable_targets & kFriendly) != 0) return refused(Refusal::NeedsTarget);
    // A song (a press, an infinite effect with a tick) is a loop: its effects start kLoopDelay on and the next press ends it. Any other ability's infinite
    // effect that no hit applies (Guard, Reflect, Tank_IronAura, a consumable's Rest) is its own: it goes on the caster with the cast, and ends with a
    // toggle's second press or a held ability's release; the ones a hit applies come with the hit (the client asks for it).
    bool loop = false;
    int64_t loop_effect = 0;
    std::vector<const EffectDef*> own_effects;
    const bool single_press = row.activation_input.find("Single") != std::string::npos;
    const std::set<int64_t> by_hits = effects_applied_by_hits(*def);
    for (const auto& [key, effect_guid] : def->effects) {
        const EffectDef* e = catalog_->effect(effect_guid);
        if (!e || !e->row.infinite_duration) continue;
        if (e->row.tick_timer > 0 && single_press) {
            loop = true;
            loop_effect = effect_guid;
        } else if (!by_hits.count(effect_guid)) {
            own_effects.push_back(e);
        }
    }
    const bool is_toggle = row.held_behavior.find("Toggle") != std::string::npos;

    // the other actors the call names: each must be there, alive, of the side the ability reaches, in range and in sight [plan E1, stage 3]
    std::vector<uint64_t> target_actors;
    if (combat_) {
        const bool melee = row.name.rfind("WeaponCombo_Melee", 0) == 0 ||
                           std::find(row.tags.begin(), row.tags.end(), "ability.Melee") != row.tags.end();
        for (const uint64_t id : info.target_actors) {
            if (target_actors.size() >= 16) break;
            const combat::TargetInfo ti = combat_->inspect(id);
            if (!ti.exists || !ti.alive) return refused(Refusal::BadTarget, " (the target is not there or is dead)");
            const bool hostile = ti.relation == combat::Relation::Hostile;
            if (hostile ? (row.applicable_targets & 2) == 0 : (row.applicable_targets & (1 | 4 | 8 | 16)) == 0) return refused(Refusal::BadTarget, hostile ? " (it does not harm enemies)" : " (it does not help allies)");
            if (ti.surface_distance_cm > combat_->reach_cm(melee) + combat_->data().activation_leeway_cm()) return refused(Refusal::OutOfRange);
            // Line of sight is NOT a refusal for a player's cast (E1 review I4): `TargetInfo::in_line_of_sight` is the navmesh's 2D raycast, which a
            // bank, a ledge or a gap in the walkable ground stops as a wall would; the client enforces its own sight rules, and retail's server refused
            // none of the 361 recorded casts. The AI's aggro keeps the raycast (combat_world.cpp).
            target_actors.push_back(id);
        }
    }
// CoA + C Y B

    // the formulas
    Cast cast;
    cast.client_id = client_id;
    cast.def = def;
    cast.trigger_at = t;
    cast.animation_index = info.animation_index;
    cast.target = info.target_count > 0 && target_actors.empty();   // (aimed at the caster; a real target is named by the item's Targets)
    // ... but a projectile reaches the caster only when the ability may be aimed at the caster at all: an enemy-only ability whose target the world
    // does not have (another player) is cast at the air, like one with no target, and Mock's Quick Wit heal never lands on the caster [E1 stage 3 fix]
    cast.self_reachable = (row.applicable_targets & kFriendly) != 0;
    cast.target_actors = target_actors;
    cast.loop = loop;
    cast.toggle_off = (loop && stacks_of(loop_effect) > 0) || (is_toggle && !own_effects.empty() && stacks_of(own_effects[0]->row.guid) > 0);   // on: this press ends it
    if (!cast.toggle_off) cast.own_effects = own_effects;
    cast.group = group_key(row);
    drop_evaluation();   // the character as it is at this trigger
    std::string why;
    auto broken = [&](const char* what) {
        no_formula_.insert(ability);
        return refused(Refusal::NoFormula, " (" + std::string(what) + ": " + why + ")");
    };
    bool ok = true;
    if (!eval_requirement(row.requirement_expr, target_actors, ok, why)) return broken("its activation requirement");
    if (!ok) return refused(Refusal::Requirement);
    if (const char* what = resolve_formulas(cast, *def, why)) return broken(what);
    if (cast.toggle_off) {   // ending a loop is free
        cast.stat_costs.clear();
        cast.effect_costs.clear();
        cast.triggers_cooldown = false;
    }
    // A chain: projectile rows that all fire the same hit (Mend's five) are the steps of the combo: a cast fires one, by the charges it has used
    // [R: capture 122, the Mend of each charge heals once, +14.8 more at each step, and Cleric_Mend_Combo's stacks are the step].
    {
        std::vector<const content::AbilityHitRow*> rows;
        std::set<int64_t> distinct;
        for (const content::AbilityHitRow& h : def->hits) {
            if (h.source != "projectile") continue;
            rows.push_back(&h);
            distinct.insert(h.hit_guid);
        }
        if (rows.size() > 1 && distinct.size() == 1) {
            const auto g = groups_.find(cast.group);
            const int left = g != groups_.end() ? g->second.charges : cast.max_charges;
            const int used = std::clamp(cast.max_charges - left, 0, static_cast<int>(rows.size()) - 1);
            cast.chain_owner = rows[static_cast<size_t>(used)]->owner_guid;
        }
    }
    // the group's charges
    if (const auto g = groups_.find(cast.group); g != groups_.end()) {
        if (g->second.charges < cast.charge_cost || (row.disable_during_cooldown && g->second.timer_end >= 0)) return refused(Refusal::Charges);
    }
    // the costs
    for (const auto& [stat, amount] : cast.stat_costs) {
        if (stat_now(stat) < amount) return refused(Refusal::Cost, " (" + std::to_string(amount) + " of stat " + std::to_string(stat) + " needed)");
    }
    for (const auto& [effect, n] : cast.effect_costs) {
        if (stacks_of(effect) < n) return refused(Refusal::Cost, " (" + std::to_string(n) + " stacks of effect " + std::to_string(effect) + " needed)");
    }

    // accepted: the steps and their times
    double item_delay = kItemDelay, cost_delay = kCostDelay, hit_delay = -1;
    double zone_delay = -1;   // when the actor of the zone the cast spawns opens, after the trigger (-1: a moment after the item)
    double zone_end = -1;     // when the dance ends, after the trigger (-1: the zone's life)
    {
        // what the retail captures show of the ability (ability_timings.h): the montage's first notify differs from one ability to the next
        const AbilityTiming* lo = std::begin(kAbilityTimings);
        const AbilityTiming* hi = std::end(kAbilityTimings);
        const AbilityTiming* it = std::lower_bound(lo, hi, ability, [](const AbilityTiming& x, int64_t g) { return x.ability < g; });
        if (data_timings_ && it != hi && it->ability == ability) {
            if (it->item_ms >= 0) item_delay = it->item_ms / 1000.0;
            if (it->cost_ms >= 0) cost_delay = it->cost_ms / 1000.0;
            else if (it->item_ms >= 0) cost_delay = item_delay;
            if (it->zone_ms >= 0) zone_delay = it->zone_ms / 1000.0;
            if (it->zone_close_ms >= 0) zone_end = it->zone_close_ms / 1000.0 - kZoneCloseDelay;   // the actor's close is the dance's end and the fade
        }
    }
    for (const Timing& tm : kTimings) {
        if (row.name == tm.ability) {
            item_delay = tm.item;
            cost_delay = tm.cost;
            hit_delay = tm.hit;
        }
    }
    bool has_montage = false;   // (an ability with no montage has a row with an empty path: Fortissimo)
    for (const content::MontageRow& m : def->montages) has_montage = has_montage || !m.soft_path.empty();
    if (has_montage) cast.item_due = t + item_delay;
    cast.cost_due = t + cost_delay;
    if (!loop) {
        bool direct = false, projectile = false;
        const bool aimed = !cast.target_actors.empty();
        for (const content::AbilityHitRow& h : def->hits) {
            const HitDef* hit = catalog_->hit(h.hit_guid);
            if (!hit) continue;
            const bool others = aimed && (hit->row.applicable_targets & (2 | 4 | 8 | 16)) != 0;
            if (!hits_caster(hit->row) && !others) continue;
            if (h.source == "hit" && hit->row.net_type.find("ServerVerify") == std::string::npos) direct = true;   // NET_ServerOnly: the server's own
            if (h.source == "projectile" && ((cast.target && cast.self_reachable) || aimed)) projectile = true;
        }
        if (direct) cast.hit_due = *cast.cost_due + (hit_delay >= 0 ? hit_delay : has_montage ? kHitDelay : 0.0);
        if (projectile) cast.projectile_due = *cast.cost_due + kProjectileDelay;
    }
    // a movement ability opens the move checker's window
    double speed = row.networking_max_speed;
    for (const Dash& d : kDashes) {
        if (row.name == d.ability) speed = std::max(speed, d.speed);
    }
    last_max_charges_[ability] = cast.max_charges;
    if (speed > 0) movement_.emplace_back(t + kMovementWindow, speed * kMovementMargin);
    movement_.erase(std::remove_if(movement_.begin(), movement_.end(), [&](const auto& m) { return m.first < t; }), movement_.end());
    ++casts_started_;
    if (loop && !cast.toggle_off) cast.loop_due = t + kLoopDelay;
    if (!cast.own_effects.empty()) cast.own_due = *cast.cost_due + kHitDelay;
    // the hits the client will ask for: every direct hit that is not aimed at the caster
    CastRecord rec;
    rec.client_id = client_id;
    rec.ability = row.guid;
    rec.trigger_at = t;
    rec.cost_due = *cast.cost_due;
    rec.window_end = rec.cost_due + kRequestWindow;
    rec.hit_delay = hit_delay >= 0 ? hit_delay : has_montage ? kHitDelay : 0.0;
    rec.reach = reach_of(*def);
    std::set<int64_t> seen;
    for (const content::AbilityHitRow& h : def->hits) {
        if (h.source != "hit" || !seen.insert(h.hit_guid).second) continue;
        const int64_t tg = targeting_of(*def, static_cast<int>(h.key));
        const HitDef* hit = catalog_->hit(h.hit_guid);
        if (!hit || hit->row.net_type.find("ServerVerify") == std::string::npos) continue;   // a NET_ServerOnly hit is no one's request
        HitSlot slot;
        slot.hit = hit;
        slot.targeting = tg;
        slot.reaches = hits_caster(hit->row);
        slot.aims_others = combat_ != nullptr && (hit->row.applicable_targets & (2 | 4 | 8 | 16)) != 0;
        rec.slots.push_back(std::move(slot));
    }
    records_.push_back(std::move(rec));
    while (records_.size() > kMaxRecords) records_.pop_front();
    // the zones the cast spawns: their actors open at retail's offset after the trigger where the captures show it, else a moment after the item
    // [R: capture 44: Maddening +362 (item +328), Nimble +300 (+300), Mesmerizing +389 (+324), Shielding +462 (+331)]; what their overlap hits put on the
    // caster at the cost step belongs to them, and the cast's record takes the client's requests for their ticks while they live
    spawn_zones(cast, t + (zone_delay >= 0 ? zone_delay : (has_montage ? item_delay : cost_delay) + kZoneOpenDelay), zone_end >= 0 ? t + zone_end : -1.0);
    casts_.push_back(std::move(cast));
    return Refusal::None;
}
// == C O A | C-Y-B ==

void AbilityPipeline::step_item(Cast& cast, double t) {
    const AbilityDef& def = *cast.def;
    AbilityChange c;
    if (item_instance_) {
        c.abilities_removed.push_back(*item_instance_);
        item_instance_.reset();
    }
    bootstrap::ActiveAbilityItem item;
    item.ability_guid = def.row.guid;
    item.server_instance_id = cast.instance = a7_.allocate_instance_id();
    instances_.push_back({cast.instance, cast.client_id, std::nullopt, def.row.guid});
    while (instances_.size() > 32) instances_.pop_front();
    item.targets_self = cast.target;
    if (combat_) {
        for (const uint64_t id : cast.target_actors) {
            item.target_objects.push_back(combat_->hit_component_of(id));
            item.target_actors.push_back(combat_->guid_of(id));
        }
    }
    for (const content::MontageRow& m : def.montages) {
        if (m.anim_index != cast.animation_index) continue;
        item.montage_path = m.soft_path;
        item.montage_guid = m.net_guid ? *m.net_guid : 0;
        break;
    }
    item.montage_group = montage_group_of(def);
    item.animation_counter = ++animation_counter_;
    c.abilities_added.push_back(std::move(item));
    item_instance_ = cast.instance;
    last_item_at_ = t;


    emit({std::move(c), std::nullopt, t, "the item of " + def.row.name});
}
// C:Y:B / C O A

void AbilityPipeline::step_cost(Cast& cast, double t) {
    const AbilityDef& def = *cast.def;
    AbilityChange c;
    // the costs again: nothing but regeneration and Sprint's drain can have moved since the trigger, and a cast that cannot pay now is over
    bool affordable = true;
    for (const auto& [stat, amount] : cast.stat_costs) affordable = affordable && stat_now(stat) >= amount;
    for (const auto& [effect, n] : cast.effect_costs) affordable = affordable && stacks_of(effect) >= n;
    if (const auto g = groups_.find(cast.group); g != groups_.end() && cast.triggers_cooldown && g->second.charges < cast.charge_cost) affordable = false;   // a cast that waited behind another's
    if (CastRecord* rec = record_of(cast.client_id)) rec->cost_at = t;
    if (!affordable) {
        drop_record(cast.client_id);
        // the zones the cast made go with it: one that did not open is dropped, one that did ends now
        for (size_t i = 0; i < zones_.size();) {
            Zone& z = zones_[i];
            if (z.client_id != cast.client_id || z.ability != def.row.guid || z.state == Zone::State::Ended) {
                ++i;
            } else if (z.state == Zone::State::Pending) {
                zones_.erase(zones_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                z.end_at = std::min(z.end_at, t);
                ++i;
            }
        }
        AbilityEvent e;
        if (item_instance_ && cast.instance >= 0 && *item_instance_ == cast.instance) {
            e.change.abilities_removed.push_back(cast.instance);
            item_instance_.reset();
        }
        e.end_ability = cast.client_id;
        e.at = t;
        e.note = def.row.name + " cannot be paid for any more: the cast is over";
        e.refusal = true;
        ++refused_;
        cast.hit_due.reset();
        cast.projectile_due.reset();
        cast.loop_due.reset();
        cast.own_due.reset();
        events_.push_back(std::move(e));
        return;
    }
    bool spent_mana = false;
    for (const auto& [stat, amount] : cast.stat_costs) {
        take_stat(c, stat, amount);
        spent_mana = spent_mana || stat == content::kGuidMana;
    }
    for (const auto& [effect, n] : cast.effect_costs) remove_stacks(c, effect, n);
    drop_evaluation();
    if (resource_ && spent_mana) apply_effect(c, *resource_, 1, std::nullopt, t);   // the class's resource: one stack per cast that spends Mana
    use_charge(c, cast, t);
    if (cast.toggle_off) {
        end_loop(c, def.row.guid, t);
        held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const Held& h) { return h.ability == def.row.guid; }), held_.end());
    }
    if (!cast.loop) {
        // the first overlap of the zones the ability spawns: the caster stands in them
        for (const content::AbilityHitRow& h : def.hits) {
            if (h.source != "zone" || (h.role != "InitialHitsIds" && h.role != "OnBeginOverlapHitsIds")) continue;
            HitSource src;   // [R: 84 zone hits in the confirmations: predicted on neither]
            src.type = content::kSourceTypeZone;
            src.guid = h.owner_guid;
            src.actor = false;
            src.confirm = true;
            src.autonomous = false;
            zone_owner_ = 0;
            for (const Zone& z : zones_) {
                if (z.client_id == cast.client_id && z.ability == def.row.guid && z.zone == h.owner_guid && z.state != Zone::State::Ended) zone_owner_ = z.id;
            }
            if (const HitDef* hit = catalog_->hit(h.hit_guid)) apply_hit(c, *hit, t, true, src);
            zone_owner_ = 0;
        }
    }
    recompute(c);
    emit({std::move(c), std::nullopt, t, "the cost, cooldown and class resource of " + def.row.name});
}
// Cyb and C O A

void AbilityPipeline::step_own(Cast& cast, double t) {
    const AbilityDef& def = *cast.def;
    AbilityChange c;
    for (const EffectDef* e : cast.own_effects) apply_effect(c, *e, 1, std::nullopt, t, false, def.row.guid);
    sync_engine_effects();
    // a held ability (the key stays down: Guard, Reflect) ends with the client's cancel
    if (def.row.hold_animation) {
        held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const Held& h) { return h.ability == def.row.guid; }), held_.end());
        held_.push_back({cast.client_id, def.row.guid, std::nullopt});
        if (held_.size() > 16) held_.erase(held_.begin());
    }
    recompute(c);
    emit({std::move(c), std::nullopt, t, "the own effects of " + def.row.name + " start"});
}

void AbilityPipeline::end_held(size_t index, double t) {
    const int64_t ability = held_[index].ability;
    held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(index));
    AbilityChange c;
    end_loop(c, ability, t);
    recompute(c);
    const AbilityDef* a = catalog_->ability(ability);
    emit({std::move(c), std::nullopt, t, "the held ability " + (a ? a->row.name : std::string("?")) + " is released"});
}

void AbilityPipeline::end_loop(AbilityChange& c, int64_t ability, double t) {
    for (size_t i = 0; i < effects_.size();) {
        if (effects_[i].owner != ability) {
            ++i;
            continue;
        }
        const EffectInst gone = effects_[i];
        remove_instance(c, i);
        run_effect_hits(c, gone, kExpirePhases, 2, t);
    }
    sync_engine_effects();
}

void AbilityPipeline::step_loop(Cast& cast, double t) {
    const AbilityDef& def = *cast.def;
    AbilityChange c;
    // a song replaces the one that plays (the songs share a tag): its effects end
    std::set<int64_t> others;
    for (const EffectInst& e : effects_) {
        if (e.owner == 0 || e.owner == def.row.guid) continue;
        const AbilityDef* a = catalog_->ability(e.owner);
        if (a && !is_none_tag(def.row.shared_cooldown_tag) && a->row.shared_cooldown_tag == def.row.shared_cooldown_tag) others.insert(e.owner);
    }
    for (const int64_t o : others) end_loop(c, o, t);
    // its effects (the loop's own, the buff ...) start now and the loop effect re-adds the others every tick
    for (const auto& [key, guid] : def.effects) {
        if (const EffectDef* e = catalog_->effect(guid)) apply_effect(c, *e, 1, std::nullopt, t, false, def.row.guid);
    }
    recompute(c);
    emit({std::move(c), std::nullopt, t, "the effects of " + def.row.name + " start"});
}

// ---- lingering zones ------------------------------------------------------------------------------------------------------------------

void AbilityPipeline::spawn_zones(const Cast& cast, double open_at, double end_at) {
    if (cast.loop) return;
    for (const int64_t guid : cast.def->zones) {
        const content::ZoneRow* row = catalog_->zone(guid);
        if (!row) continue;
        Zone z;
        z.id = next_zone_id_++;
        z.ability = cast.def->row.guid;
        z.zone = guid;
        z.client_id = cast.client_id;
        z.open_at = open_at;
        const double life = row->life_time > 0 ? row->life_time : 30.0;
        // the dance ends where the captures show (ability_timings.h), else when the zone's life in the data is over
        z.end_at = end_at >= 0 ? std::max(end_at, open_at) : open_at + life;
        zones_.push_back(z);
        if (zones_.size() > kMaxZones) zones_.erase(zones_.begin());
        // the client asks for the zone's tick hits as long as its actor lives
        if (CastRecord* rec = record_of(cast.client_id)) {
            rec->window_end = std::max(rec->window_end, z.end_at + kZoneRequestSlack);
            rec->max_requests = std::max(rec->max_requests, static_cast<int>(life / std::max(0.25, row->tick_rate)) + kZoneExtraRequests);
        }
    }
}

double AbilityPipeline::zone_due(const Zone& z) const {
    switch (z.state) {
        case Zone::State::Pending: return z.open_at;
        case Zone::State::Open: return z.end_at;
        case Zone::State::Ended: return std::min(z.fade_at >= 0 ? z.fade_at : kInf, z.close_at);
    }
    return kInf;
}


void AbilityPipeline::end_zone_effects(AbilityChange& c, int64_t zone_id, double t) {
    for (size_t i = 0; i < effects_.size();) {
        if (effects_[i].zone != zone_id || !std::isinf(effects_[i].end)) {
            ++i;
            continue;
        }
        const EffectInst gone = effects_[i];
        remove_instance(c, i);
        run_effect_hits(c, gone, kExpirePhases, 2, t);
    }
    sync_engine_effects();
}

void AbilityPipeline::run_zone(size_t index, double t) {
    Zone& z = zones_[index];
    AbilityEvent e;
    e.at = t;
    bootstrap::ZoneOp op;
    op.id = z.id;
    op.ability = z.ability;
    op.zone = z.zone;
    const AbilityDef* a = catalog_->ability(z.ability);
    const std::string who = a ? a->row.name : std::string("?");
    switch (z.state) {
        case Zone::State::Pending:
            z.state = Zone::State::Open;
            op.kind = bootstrap::ZoneOp::Kind::Open;
            e.zones.push_back(op);
            e.note = "the zone of " + who + " opens";
            emit(std::move(e));
            return;
        case Zone::State::Open: {
            AbilityChange c;
            end_zone_effects(c, z.id, t);
            recompute(c);
            z.state = Zone::State::Ended;
            z.fade_at = t + kZoneFadeDelay;
            z.close_at = t + kZoneCloseDelay;
            if (CastRecord* rec = record_of(z.client_id)) rec->window_end = std::max(rec->cost_due + kRequestWindow, t + kZoneRequestSlack);
            e.change = std::move(c);
            e.note = "the dance of " + who + " ends";
            emit(std::move(e));
            return;
        }
        case Zone::State::Ended:
            if (z.fade_at >= 0 && z.fade_at <= t) {
                z.fade_at = -1;
                op.kind = bootstrap::ZoneOp::Kind::Fade;
                e.zones.push_back(op);
                e.note = "the zone of " + who + " fades out";
                emit(std::move(e));
                return;
            }
            op.kind = bootstrap::ZoneOp::Kind::Close;
            e.zones.push_back(op);
            e.note = "the zone of " + who + " closes";
            zones_.erase(zones_.begin() + static_cast<std::ptrdiff_t>(index));
            emit(std::move(e));
            return;
    }
}
// [Cyb - C | O | A]

void AbilityPipeline::step_hits(Cast& cast, double t, const char* which) {
    const AbilityDef& def = *cast.def;
    AbilityChange c;
    // E1 stage 3 fix 2: a weapon combo's projectile step fires the projectile of the swing's step (the montage the client's AnimationIndex named), not
    // every projectile the combo lists (ten for the book, six for the longbow), and the step's direct hits that no projectile carries but that fire one
    // when they land (the book's Split hits: the damage) go with it [R: research/hitfx 4 and 9]
    const bool combo_step = std::string_view(which) == "projectile" && is_weapon_combo(def.row);
    StepToken step;
    if (combo_step) {
        const content::MontageRow* montage = nullptr;
        for (const content::MontageRow& m : def.montages) {
            if (m.anim_index == cast.animation_index) montage = &m;
        }
        if (!montage && !def.montages.empty()) montage = &def.montages.front();
        if (montage) step = step_token(leaf_of_path(montage->soft_path));
        if (step.beam) {   // the wand's beam: its hits are the client's requests
            emit({std::move(c), std::nullopt, t, "the projectile step of " + def.row.name + " (a beam: nothing flies)"});
            return;
        }
    }
    std::vector<const HitDef*> fired;   // the projectile hits of the step (for their paired direct hits)
    std::vector<int64_t> fired_by;      // and the projectile record that carried each (the paired hit's SourceRecordGuid)
    for (const content::AbilityHitRow& h : def.hits) {
        if (h.source != which) continue;
        const HitDef* hit = catalog_->hit(h.hit_guid);
        if (!hit) continue;
        HitSource src;
        if (std::string_view(which) == "hit") {
            // only the server's own hits (NET_ServerOnly): they apply with the cast; every NET_ServerVerify hit is the client's to ask for
            // (RequestHitVerification_Server: it picks the hits its state needs, a toggle's second press asks for the removing one) and applies when it does
            if (hit->row.net_type.find("ServerVerify") != std::string::npos) continue;
            src.type = content::kSourceTypeAbility;
            src.guid = def.row.guid;
            src.confirm = true;
        } else {
            if (cast.chain_owner != 0 && h.owner_guid != cast.chain_owner) continue;
            if (combo_step) {
                const StepToken mine = step_token(hit->row.name);
                if (!mine.ok || (step.ok && !same_step(step, mine))) continue;
                fired.push_back(hit);
                fired_by.push_back(h.owner_guid);
            }
            src.type = content::kSourceTypeProjectile;   // a projectile's hit has no Source and is confirmed by the server [R: 3,529 of them]
            src.guid = h.owner_guid;
            src.actor = false;
            src.confirm = true;
            src.simulated = true;
        }
        if (!cast.target_actors.empty() && (hit->row.applicable_targets & (2 | 4 | 8 | 16)) != 0) apply_hit_on_targets(c, *hit, cast.target_actors, t, src, &def);
        // A projectile flies to its target: its hits land on the actor it reaches, the caster only when the cast aimed at nobody else (Jest on oneself).
        // Mock lists the Quick Wit projectile too (both abilities list both: Mockery and Quick Wit), and its Bard_QuickWit_Apply heal names Self and Ally
        // [R: capture 40 line 295, Mock at another actor: Bard_Mockery_Apply alone is confirmed, no Bard_QuickWit_Apply, no Glee; the user's run of
        // 2026-10-06: Mock healed the caster]. A direct hit (NET_ServerOnly, "hit") is around the caster and keeps reaching it.
        const bool at_others = std::string_view(which) == "projectile" && !cast.target_actors.empty();
        if (hits_caster(hit->row) && !at_others) apply_hit(c, *hit, t, true, src);
    }
    // the step's direct hits that land with the projectile: carried by no projectile of the combo, firing one of their own (the book's Split hits),
    // of the same step as a projectile hit that flew; confirmed in the same confirmation, predicted on neither side [R: pa 0 ps 0, 408 of 408]
    if (combo_step && !fired.empty() && !cast.target_actors.empty()) {
        std::set<int64_t> carried;
        for (const content::AbilityHitRow& h : def.hits) {
            if (h.source == "projectile") carried.insert(h.hit_guid);
        }
        std::set<int64_t> done;
        for (const content::AbilityHitRow& h : def.hits) {
            if (h.source != "hit" || carried.count(h.hit_guid) || !done.insert(h.hit_guid).second) continue;
            const HitDef* hit = catalog_->hit(h.hit_guid);
            if (!hit || hit->fires.empty() || (hit->row.applicable_targets & (2 | 4 | 8 | 16)) == 0) continue;
            const StepToken mine = step_token(hit->row.name);
            int64_t landed = 0;   // the projectile of the step that carried the hit this one lands with
            for (size_t i = 0; i < fired.size() && landed == 0; ++i) {
                const StepToken theirs = step_token(fired[i]->row.name);
                if (same_step(mine, theirs) && mine.variant == theirs.variant) landed = fired_by[i];
            }
            if (landed == 0) continue;
            // E1 stage 3 fix 3: its SourceRecordGuid is the projectile that LANDED (Weapon_Book_Projectile_n), not the one it fires, and its Source is
            // the target actor the projectile landed on: where the client spawns the fired projectile from (EAbilitySystemOrigin::Source; the user's
            // client logged "Failed to find origin location ... Source , Target CollisionCylinder" for every swing while the Source was empty)
            // [R: 45 packet 148: Book_Projectile_Split_Hit_1 {SourceRecordGuid Weapon_Book_Projectile_1, Source = the Wolf actor, pa 0 ps 0}]
            HitSource src;
            src.type = content::kSourceTypeProjectile;
            src.guid = landed;
            src.actor = false;
            src.confirm = true;
            src.simulated = false;
            src.autonomous = false;
            src.source_is_target = true;
            apply_hit_on_targets(c, *hit, cast.target_actors, t, src, &def);
        }
    }
    recompute(c);
    emit({std::move(c), std::nullopt, t, std::string("the ") + (std::string_view(which) == "hit" ? "hits" : "projectile hits") + " of " + def.row.name});
}

}  // namespace aoc::worldd

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/character_stats.cpp (see character_stats.h)
// ============================================================================
#include "stats/character_stats.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>

namespace aoc::worldd::stats {

namespace {

uint32_t bits_of(float f) { return std::bit_cast<uint32_t>(f); }

// The float the client holds for a number of the engine. One that is not finite or beyond a float32 (the engine settles its numbers as
// float32 and counts that as a fault already; this keeps a double-mode bundle from getting an undefined conversion through) is a fault
// and 0, never an infinity or an undefined conversion.
float to_float(double d, int& faults) {
    if (!(std::fabs(d) <= static_cast<double>(std::numeric_limits<float>::max()))) {
        ++faults;
        return 0.0f;
    }
    return static_cast<float>(d);
}

// What the formulas ask of the character besides its stats: whether it moves and whether it is in combat (the other answers are the
// engine's "none": the effects and the ranks come with the inputs).
class FlagState final : public StatState {
public:
    FlagState(bool moving, bool in_combat, const NpcShape& npc = {}) : moving_(moving), in_combat_(in_combat), npc_(npc) {}
    bool in_combat() const override { return in_combat_; }
    bool moving() const override { return moving_; }
    double npc_elite_rank() const override { return npc_.elite_rank; }
    double npc_stat_distribution(int i) const override { return i >= 0 && i < 8 ? npc_.dist[static_cast<size_t>(i)] : 0; }

private:
    bool moving_, in_combat_;
    NpcShape npc_;
};

}  // namespace

struct CharacterStats::Evaluation::State {
    FlagState flags;
    State(bool moving, bool in_combat, const NpcShape& npc) : flags(moving, in_combat, npc) {}
};

CharacterStats::Evaluation::Evaluation(const StatBundle& bundle, StatInputs inputs, bool moving, bool in_combat, const NpcShape& npc)
    : bundle_(&bundle), in_(std::move(inputs)), state_(std::make_unique<State>(moving, in_combat, npc)) {
    if (!run_stats(*bundle_, in_, state_->flags, result_, err_)) return;
    ev_ = std::make_unique<ExprEvaluator>(*bundle_, in_, state_->flags, result_);
    if (!ev_->ok()) err_ = ev_->error();
}

CharacterStats::Evaluation::~Evaluation() = default;

bool CharacterStats::Evaluation::eval(int64_t program, ExprHost& host, double& out, std::string& err) {
    if (!ev_ || !ok()) {
        err = err_;
        return false;
    }
    return ev_->eval(program, host, out, err);
}

double CharacterStats::Evaluation::stat(int64_t guid) const {
    for (const StatOut& s : result_.stats) {
        if (s.guid == guid) return s.value;
    }
    return 0;
}
// -- C:Y:B x CoA --

int CharacterStats::Evaluation::faults() const { return result_.faults + (ev_ ? ev_->faults() : 0); }

const StatState& CharacterStats::Evaluation::state() const { return state_->flags; }

bool CharacterStats::Evaluation::set_actor(int role, const Evaluation& other, std::string& err) {
    if (!ev_ || !ok()) {
        err = err_;
        return false;
    }
    if (!other.ok()) {
        err = other.err_;
        return false;
    }
    return ev_->set_actor(role, other.in_, other.state_->flags, other.result_, err);
}

void CharacterStats::Evaluation::clear_actors() {
    if (ev_) ev_->clear_actors();
}

float clamp_current(float current, float max) {
    // not std::min: with a NaN current it would keep the NaN, and the order of its arguments decides which of +0.0 and -0.0 it returns
    return (max < current || current != current) ? max : current;
}

bool same_on_the_wire(const StatValue& a, const StatValue& b) {
    if (bits_of(a.value) != bits_of(b.value)) return false;
    if (a.buckets() && (bits_of(a.base) != bits_of(b.base) || bits_of(a.equip) != bits_of(b.equip))) return false;
    return true;
}

CharacterStats::CharacterStats(const StatBundle& bundle, const GearCatalog& gear, int class_enum, int level, Rounding rounding)
    : bundle_(&bundle), gear_(&gear), class_(class_enum), level_(level), rounding_(rounding) {}

bool CharacterStats::compute(std::span<const WornItem> worn, Next& next, std::string& err) const {
    const GearInputs g = gear_inputs(*gear_, worn, rounding_);
    StatInputs in;
    in.class_enum = class_;
    in.level = level_;
    in.mods = g.mods;
    in.equipped = g.equipped;
    // plan D2: the effect state: the ranks' passives (plan D1's constant mods, an infinite effect each), then the ability runtime's effects;
    // the engine applies them after the equipment, each in its stat's form
    in.effects = passives_;
    in.effects.insert(in.effects.end(), effects_.begin(), effects_.end());
    in.ranks = ranks_;
    for (const StatValue& v : values_) {
        if (v.resource()) in.current.emplace_back(v.guid, static_cast<double>(v.value));
    }
    StatResult r;
    if (!run_stats(*bundle_, in, FlagState(moving_, in_combat_, npc_), r, err)) return false;
    std::vector<StatValue>& out = next.values;
    out.clear();
    out.reserve(r.stats.size());
    int faults = r.faults;
    for (const StatOut& s : r.stats) {
        StatValue v;
        v.guid = s.guid;
        v.replication = s.replication;
        v.flags = s.flags;
        v.value = to_float(s.value, faults);
        v.base = to_float(s.base, faults);
        v.equip = to_float(s.equip, faults);
        v.has_max = s.has_max;
        v.has_min = s.has_min;
        v.has_regen = s.has_regen;
        if (s.has_max) v.max = to_float(s.max, faults);
        if (s.has_min) v.min = to_float(s.min, faults);
        if (s.has_regen) v.regen = to_float(s.regen, faults);
        if (resources_.count(s.guid)) v.flags |= kStatResource;
        out.push_back(v);
    }
    next.faults = faults;
    next.ignored_mods = r.ignored_mods;
    next.unknown_items = g.unknown_items;
    next.unknown_sets = g.unknown_sets;
    return true;
}

bool CharacterStats::start(std::span<const WornItem> worn, std::string& err) {
    Next next;
    if (!compute(worn, next, err)) return false;
    commit(std::move(next));
    started_ = true;
    return true;
}

void CharacterStats::commit(Next&& next) {
    values_ = std::move(next.values);
    faults_ = next.faults;
    ignored_mods_ = next.ignored_mods;
    unknown_items_ = next.unknown_items;
    unknown_sets_ = next.unknown_sets;
}

bool CharacterStats::prepare(std::span<const WornItem> worn, std::vector<StatValue>& changed, Next& next, std::string& err) const {
    changed.clear();
    if (!started_) {
        err = "the stats were not started";
        return false;
    }
    Next n;
    if (!compute(worn, n, err)) return false;
    std::vector<StatValue>& now = n.values;
    // by stat guid: the stats of one class are in the same order every run, but a class change (plan D1) lists other stats (the
    // class's own resource); a stat the client never had counts as changed, one it had and the new class lacks is left as it is
    std::unordered_map<int64_t, const StatValue*> had;
    had.reserve(values_.size());
    for (const StatValue& v : values_) had[v.guid] = &v;
    for (StatValue& v : now) {
        const auto it = had.find(v.guid);
        if (it == had.end()) {
            changed.push_back(v);
            continue;
        }
        if (v.resource()) {
            // a class resource: its current value stays, between the limits the run found
            float cur = it->second->value;
            if (v.has_max && cur > v.max) cur = v.max;
            if (v.has_min && cur < v.min) cur = v.min;
            v.value = cur;
        } else if (v.held()) {
            v.value = clamp_current(it->second->value, v.value);
        }
        if (!same_on_the_wire(*it->second, v)) changed.push_back(v);
    }
    next = std::move(n);
    return true;
}

bool CharacterStats::update(std::span<const WornItem> worn, std::vector<StatValue>& changed, std::string& err) {
    Next next;
    if (!prepare(worn, changed, next, err)) return false;
    commit(std::move(next));
    return true;
}
// ~ C:Y:B :: C O A ~

void CharacterStats::set_resources(std::set<int64_t> guids) {
    resources_ = std::move(guids);
    for (StatValue& v : values_) {
        if (resources_.count(v.guid)) v.flags |= kStatResource;
        else v.flags = static_cast<uint8_t>(v.flags & ~kStatResource);
    }
}

void CharacterStats::set_current(int64_t guid, float value) {
    for (StatValue& v : values_) {
        if (v.guid == guid) {
            if (v.held()) v.value = value;
            return;
        }
    }
}


std::unique_ptr<CharacterStats::Evaluation> CharacterStats::evaluation(std::span<const WornItem> worn, std::string& err, bool live_vitals) const {
    const GearInputs g = gear_inputs(*gear_, worn, rounding_);
    StatInputs in;
    in.class_enum = class_;
    in.level = level_;
    in.mods = g.mods;
    in.equipped = g.equipped;
    in.effects = passives_;
    in.effects.insert(in.effects.end(), effects_.begin(), effects_.end());
    in.ranks = ranks_;
    for (const StatValue& v : values_) {
        if (v.resource() || (live_vitals && v.held() && !v.float_array())) in.current.emplace_back(v.guid, static_cast<double>(v.value));
    }
    auto e = std::make_unique<Evaluation>(*bundle_, std::move(in), moving_, in_combat_, npc_);
    if (!e->ok()) {
        err = e->error();
        return nullptr;
    }
    return e;
}

const StatValue* CharacterStats::find(int64_t guid) const {
    for (const StatValue& v : values_) {
        if (v.guid == guid) return &v;
    }
    return nullptr;
}

}  // namespace aoc::worldd::stats

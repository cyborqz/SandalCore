// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/stat_engine.cpp: the interpreter and the stat engine
//
//  Mirrors services/internal/statprog/engine.go operation for operation: the same
//  order of arithmetic, the same rounding, the same faults, so that both give the same bits.
// ============================================================================
#include "stats/stat_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

// The arithmetic of a cubic segment and of the damage formulas is one operation per statement, in the order the Go engine does them: no fused multiply-add.
#if defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace aoc::worldd::stats {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// rnd32 is float64(float32(x)) as Go computes it: an overflow is an infinity.
double rnd32(double x) {
    if (x != x) return x;
    if (x >= 3.4028235677973366e38) return kInf;
    if (x <= -3.4028235677973366e38) return -kInf;
    return static_cast<double>(static_cast<float>(x));
}

// go_min / go_max are Go's math.Min / math.Max (NaN and signed zeros included).
double go_min(double x, double y) {
    if (x == -kInf || y == -kInf) return -kInf;
    if (std::isnan(x) || std::isnan(y)) return std::numeric_limits<double>::quiet_NaN();
    if (x == 0 && x == y) return std::signbit(x) ? x : y;
    return x < y ? x : y;
}
double go_max(double x, double y) {
    if (x == kInf || y == kInf) return kInf;
    if (std::isnan(x) || std::isnan(y)) return std::numeric_limits<double>::quiet_NaN();
    if (x == 0 && x == y) return std::signbit(x) ? y : x;
    return x > y ? x : y;
}

double arith(uint8_t o, double a, double b, bool single) {
    double r = 0;
    switch (o) {
        case op::Add: r = a + b; break;
        case op::Sub: r = a - b; break;
        case op::Mul: r = a * b; break;
        case op::Div: r = a / b; break;
        default: break;
    }
    return single ? rnd32(r) : r;
}
// C | O | A / Cyb

// cubic_segment is aocexpr.CubicSegment: FRichCurve's cubic evaluation of the segment from (t0, v0) leaving with slope leave0 to (t1, v1) arriving with
// slope arrive1, at t: the Bezier through v0, v0 + leave0 * dt / 3, v1 - arrive1 * dt / 3 and v1 as three levels of linear interpolation.
double cubic_lerp(double a, double b, double x) {
    const double d = b - a;
    const double m = x * d;
    return a + m;
}

double cubic_segment(double t0, double v0, double leave0, double t1, double v1, double arrive1, double t) {
    const double diff = t1 - t0;
    const double alpha = (t - t0) / diff;
    const double third = 1.0 / 3.0;
    const double d0 = leave0 * diff;
    const double d1 = arrive1 * diff;
    const double o0 = d0 * third;
    const double o1 = d1 * third;
    const double c0 = v0 + o0;
    const double c1 = v1 - o1;
    const double p01 = cubic_lerp(v0, c0, alpha);
    const double p12 = cubic_lerp(c0, c1, alpha);
    const double p23 = cubic_lerp(c1, v1, alpha);
    const double p012 = cubic_lerp(p01, p12, alpha);
    const double p123 = cubic_lerp(p12, p23, alpha);
    return cubic_lerp(p012, p123, alpha);
}

// go_int64 is Go's int64(x) on amd64: a NaN or a number outside the range is the most negative value.
int64_t go_int64(double x) {
    if (x != x || x >= 9223372036854775808.0 || x < -9223372036854775808.0) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(x);
}

// eval_curve is the curve at t; fault is set for a NaN t.
double eval_curve(const Curve& c, double t, bool& fault) {
    const std::vector<CurveKey>& ks = c.keys;
    fault = false;
    if (t != t) {
        fault = true;
        return ks[0].value;
    }
    if (t <= ks[0].time) return ks[0].value;
    if (t >= ks.back().time) return ks.back().value;
    // the first key after t; the segment is the one before it
    const size_t next = static_cast<size_t>(
        std::upper_bound(ks.begin(), ks.end(), t, [](double v, const CurveKey& k) { return v < k.time; }) - ks.begin());
    const CurveKey& a = ks[next - 1];
    const CurveKey& b = ks[next];
    if (t == a.time || a.interp == kInterpConstant) return a.value;
    if (a.interp == kInterpCubic) return cubic_segment(a.time, a.value, a.leave, b.time, b.value, b.arrive, t);
    const double dv = b.value - a.value;
    const double dt = t - a.time;
    const double prod = dv * dt;
    const double den = b.time - a.time;
    const double q = prod / den;
    return a.value + q;
}

// The bound on the multiplications a run does for its effects (stacks times mods, summed): statprog's maxEffectApplications.
constexpr size_t kMaxEffectApplications = 1u << 20;

// check_effects refuses effects a run cannot take: stacks outside 0..kMaxEffectStacks, more than kMaxActiveEffects effects, or more
// multiplications than kMaxEffectApplications.
bool check_effects(const std::vector<ActiveEffect>& fx, std::string& err) {
    if (fx.size() > kMaxActiveEffects) {
        err = std::to_string(fx.size()) + " active effects (the most is " + std::to_string(kMaxActiveEffects) + ")";
        return false;
    }
    size_t apps = 0;
    for (size_t i = 0; i < fx.size(); ++i) {
        if (fx[i].stacks < 0 || fx[i].stacks > kMaxEffectStacks) {
            err = "effect " + std::to_string(i) + " has " + std::to_string(fx[i].stacks) + " stacks (0 to " + std::to_string(kMaxEffectStacks) + ")";
            return false;
        }
        apps += static_cast<size_t>(fx[i].stacks) * fx[i].mods.size();
        if (apps > kMaxEffectApplications) {
            err = "the effects ask for more than " + std::to_string(kMaxEffectApplications) + " applications";
            return false;
        }
    }
    return true;
}
// [C O A x C:Y:B]

// EffectApp is one mod of one effect, for a stat.
struct EffectApp {
    double value;
    int stacks;
};

// apply_effects applies the effect mods of a stat to its value, in the stat's form: added times the stacks to an additive stat,
// multiplied once per stack into a multiplying one, by the mod or by one minus it (a complement).
double apply_effects(double v, uint8_t flags, const std::vector<EffectApp>& apps, bool single) {
    for (const EffectApp& a : apps) {
        if ((flags & kStatMulMods) == 0) {
            v = arith(op::Add, v, arith(op::Mul, a.value, static_cast<double>(a.stacks), single), single);
        } else {
            double f = a.value;
            if (flags & kStatMulCompl) f = arith(op::Sub, 1.0, a.value, single);
            for (int k = 0; k < a.stacks; ++k) v = arith(op::Mul, v, f, single);
        }
    }
    return v;
}

uint16_t rd16(const std::vector<uint8_t>& code, size_t at) { return static_cast<uint16_t>(code[at] | (code[at + 1] << 8)); }

// ActorCtx is what the reads of one actor use: its settled stat values, its level, its state and its inputs.
struct ActorCtx {
    const std::vector<double>* vals = nullptr;
    double level = 0;
    const StatState* state = nullptr;
    const StatInputs* in = nullptr;
};

class Machine {
public:
    Machine(const StatBundle& b, const StatInputs& in, const StatState& st, size_t nstats)
        : b_(b), vals_(nstats, 0.0) {
        caster_.vals = &vals_;
        caster_.level = static_cast<double>(in.level);
        caster_.state = &st;
        caster_.in = &in;
        cur_ = &caster_;
    }

    // use_actor makes the reads that follow read the actor with the role: its own context when one was supplied, else the caster's.
    void use_actor(size_t role) { cur_ = (role < kMaxRoles && actors_[role].vals) ? &actors_[role] : &caster_; }
    void set_actor(size_t role, const ActorCtx& c) { actors_[role] = c; }
    void clear_actors() {
        for (ActorCtx& a : actors_) a = ActorCtx{};
        cur_ = &caster_;
    }

    std::vector<double>& vals() { return vals_; }
    void set_host(ExprHost* h) { host_ = h; }
    void reset_budget() {
        used_ = 0;
        exhausted_ = false;
    }
    void set_total(double v) { total_ = v; }
    void count_fault() { ++faults_; }
    int faults() const { return faults_; }
    size_t used() const { return used_; }
    bool exhausted() const { return exhausted_; }

    // settle makes a stat's value the number it is stored as, and a value that is not finite 0.
    double settle(double v) {
        if (b_.mode != Mode::Double) v = rnd32(v);
        if (std::isnan(v) || std::isinf(v)) {
            ++faults_;
            return 0;
        }
        return v;
    }

    double run(const Program& p) {
        const bool single = b_.mode == Mode::Single;
        const std::vector<uint8_t>& code = p.code;
        size_t sp = 0;
        for (size_t i = 0; i < p.locals; ++i) locals_[i] = 0;
        size_t pc = 0, steps = 0;
        for (;;) {
            // A verified program runs at most one instruction per code byte (jumps go forward).
            if (++steps > code.size()) {
                ++faults_;
                return 0;
            }
            if (++used_ > kMaxRunInstructions) {
                exhausted_ = true;
                return 0;
            }
            const uint8_t o = code[pc];
            switch (o) {
                case op::Const: stack_[sp++] = b_.consts[rd16(code, pc + 1)]; pc += 3; break;
                case op::Load: stack_[sp++] = locals_[code[pc + 1]]; pc += 2; break;
                case op::Store: locals_[code[pc + 1]] = stack_[--sp]; pc += 2; break;
                case op::Stat: stack_[sp++] = (*cur_->vals)[rd16(code, pc + 1)]; pc += 3; break;
                case op::Total: stack_[sp++] = total_; pc += 1; break;
                case op::Level: stack_[sp++] = cur_->level; pc += 1; break;
                case op::Pop: --sp; pc += 1; break;
                case op::Add: case op::Sub: case op::Mul:
                    --sp;
                    stack_[sp - 1] = arith(o, stack_[sp - 1], stack_[sp], single);
                    pc += 1;
                    break;
                case op::Div:
                    --sp;
                    if (stack_[sp] == 0) {
                        ++faults_;
                        stack_[sp - 1] = 0;
                    } else {
                        stack_[sp - 1] = arith(o, stack_[sp - 1], stack_[sp], single);
                    }
                    pc += 1;
                    break;
                case op::Mod:
                    --sp;
                    if (go_int64(stack_[sp]) == 0) {
                        ++faults_;
                        stack_[sp - 1] = 0;
                    } else {
                        const int64_t d = go_int64(stack_[sp]);
                        stack_[sp - 1] = d == -1 ? 0.0 : static_cast<double>(go_int64(stack_[sp - 1]) % d);
                    }
                    pc += 1;
                    break;
                case op::Neg: stack_[sp - 1] = -stack_[sp - 1]; pc += 1; break;
                case op::Not: stack_[sp - 1] = stack_[sp - 1] == 0 ? 1.0 : 0.0; pc += 1; break;
                case op::Lt: case op::Le: case op::Gt: case op::Ge: case op::Eq: case op::Ne: {
                    --sp;
                    const double a = stack_[sp - 1], c = stack_[sp];
                    bool r = false;
                    switch (o) {
                        case op::Lt: r = a < c; break;
                        case op::Le: r = a <= c; break;
                        case op::Gt: r = a > c; break;
                        case op::Ge: r = a >= c; break;
                        case op::Eq: r = a == c; break;
                        default: r = a != c; break;
                    }
                    stack_[sp - 1] = r ? 1.0 : 0.0;
                    pc += 1;
                    break;
                }
                case op::Jmp: pc = rd16(code, pc + 1); break;
                case op::Jz: --sp; pc = stack_[sp] == 0 ? rd16(code, pc + 1) : pc + 3; break;
                case op::Jnz: --sp; pc = stack_[sp] != 0 ? rd16(code, pc + 1) : pc + 3; break;
                case op::Round: stack_[sp - 1] = std::floor(stack_[sp - 1] + 0.5); pc += 1; break;
                case op::Floor: stack_[sp - 1] = std::floor(stack_[sp - 1]); pc += 1; break;
                case op::Ceil: stack_[sp - 1] = std::ceil(stack_[sp - 1]); pc += 1; break;
                case op::Min: --sp; stack_[sp - 1] = go_min(stack_[sp - 1], stack_[sp]); pc += 1; break;
                case op::Max: --sp; stack_[sp - 1] = go_max(stack_[sp - 1], stack_[sp]); pc += 1; break;
                case op::Sel:
                    sp -= 2;
                    stack_[sp - 1] = stack_[sp - 1] != 0 ? stack_[sp] : stack_[sp + 1];
                    pc += 1;
                    break;
                case op::Lerp: {
                    sp -= 2;
                    const double d = arith(op::Sub, stack_[sp], stack_[sp - 1], single);
                    const double pr = arith(op::Mul, stack_[sp + 1], d, single);
                    stack_[sp - 1] = arith(op::Add, stack_[sp - 1], pr, single);
                    pc += 1;
                    break;
                }
                case op::Abs: stack_[sp - 1] = std::fabs(stack_[sp - 1]); pc += 1; break;
                case op::Curve: {
                    bool f = false;
                    double v = eval_curve(b_.curves[rd16(code, pc + 1)], stack_[sp - 1], f);
                    if (f) ++faults_;
                    stack_[sp - 1] = single ? rnd32(v) : v;
                    pc += 3;
                    break;
                }
                case op::Effect: stack_[sp++] = effect_count(b_.refs[rd16(code, pc + 1)]); pc += 3; break;
                case op::EffectTag: stack_[sp++] = effect_count_by_tag(b_.strings[rd16(code, pc + 1)]); pc += 3; break;
                case op::Skill:
                    stack_[sp++] = skill_purchased(b_.refs[rd16(code, pc + 1)], b_.refs[rd16(code, pc + 3)]) ? 1.0 : 0.0;
                    pc += 5;
                    break;
                case op::Combat: stack_[sp++] = cur_->state->in_combat() ? 1.0 : 0.0; pc += 1; break;
                case op::Moving: stack_[sp++] = cur_->state->moving() ? 1.0 : 0.0; pc += 1; break;
                case op::Unkillable: stack_[sp++] = cur_->state->unkillable() ? 1.0 : 0.0; pc += 1; break;
                case op::EquipTag:
                    stack_[sp++] = equip_tag(b_.strings[rd16(code, pc + 1)], b_.strings[rd16(code, pc + 3)]) ? 1.0 : 0.0;
                    pc += 5;
                    break;
                case op::Profession:
                    stack_[sp++] = cur_->state->profession_level(cur_->state->actor_guid(), b_.strings[rd16(code, pc + 1)]);
                    pc += 3;
                    break;
                case op::Consumed: stack_[sp++] = cur_->state->consumed_item_stat(b_.refs[rd16(code, pc + 1)]); pc += 3; break;
                case op::NpcRank: stack_[sp++] = cur_->state->npc_elite_rank(); pc += 1; break;
                case op::NpcDist: stack_[sp++] = cur_->state->npc_stat_distribution(rd16(code, pc + 1)); pc += 3; break;
                case op::Actor: use_actor(code[pc + 1]); pc += 2; break;
                case op::Progress: stack_[sp++] = host_->ability_progress(b_.strings[rd16(code, pc + 1)]); pc += 3; break;
                case op::EffectInst: stack_[sp++] = host_->effect_count_by_instigator(b_.refs[rd16(code, pc + 1)]); pc += 3; break;
                case op::Rand: stack_[sp++] = host_->rand_double(); pc += 1; break;
                case op::HitTag: stack_[sp++] = host_->hit_has_tag(b_.strings[rd16(code, pc + 1)]) ? 1.0 : 0.0; pc += 3; break;
                case op::HitFlag: stack_[sp++] = host_->hit_has_flag(b_.strings[rd16(code, pc + 1)]) ? 1.0 : 0.0; pc += 3; break;
                case op::XpDebt: stack_[sp++] = host_->xp_debt_ratio(); pc += 1; break;
                case op::HitTargets: stack_[sp++] = host_->hit_targets(); pc += 1; break;
                case op::Host0: case op::Host1: case op::Host2: {
                    const int n = code[pc] - op::Host0;
                    double args[2] = {0, 0};
                    for (int i = n - 1; i >= 0; --i) args[i] = stack_[--sp];
                    stack_[sp++] = host_->host_fn(rd16(code, pc + 1), b_.strings[rd16(code, pc + 3)], args, n);
                    pc += 5;
                    break;
                }
                case op::Ret: {
                    const double v = stack_[sp - 1];
                    return b_.mode != Mode::Double ? rnd32(v) : v;
                }
                default:  // not reachable in a verified program
                    ++faults_;
                    return 0;
            }
        }
    }

private:
    // GetEffectCount: the stacks of the active effects with the record, and what the state adds (the state's first).
    double effect_count(const RecordRef& r) const {
        double n = cur_->state->effect_count(r);
        for (const ActiveEffect& e : cur_->in->effects)
            if (e.effect == r) n += static_cast<double>(e.stacks);
        return n;
    }
    // GetEffectCountByTag: the stacks of the active effects with a tag that matches, and what the state adds.
    double effect_count_by_tag(const std::string& tag) const {
        double n = cur_->state->effect_count_by_tag(tag);
        for (const ActiveEffect& e : cur_->in->effects)
            for (const std::string& t : e.tags)
                if (tag_matches(t, tag)) {
                    n += static_cast<double>(e.stacks);
                    break;
                }
        return n;
    }
    // GetSkillPurchased: a held rank, or the state says so.
    bool skill_purchased(const RecordRef& cls, const RecordRef& skill) const {
        for (const SkillRank& r : cur_->in->ranks)
            if (r.cls == cls && r.skill == skill) return true;
        return cur_->state->skill_purchased(cls, skill);
    }
    bool equip_tag(const std::string& slot, const std::string& tag) const {
        for (const EquippedTag& e : cur_->in->equipped)
            if (e.slot == slot && tag_matches(e.tag, tag)) return true;
        return cur_->state->equipped_item_has_tag(slot, tag);
    }

    const StatBundle& b_;
    ExprHost* host_ = nullptr;
    std::vector<double> vals_;
    ActorCtx caster_;
    ActorCtx actors_[kMaxRoles];
    const ActorCtx* cur_ = nullptr;
    double total_ = 0;
    int faults_ = 0;
    size_t used_ = 0;
    bool exhausted_ = false;
    double locals_[kMaxLocals] = {};
    double stack_[kMaxStack] = {};
};

}  // namespace

// Gameplay tags are FNames: compared without regard to case ("pet.summon" is "Pet.Summon": Summoner_Amplify's requirement and the pet effect's tag).
static bool tag_prefix_equal(const std::string& a, const std::string& b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
        if (x != y && std::tolower(x) != std::tolower(y)) return false;
    }
    return true;
}
// ~ CoA & C Y B ~

bool tag_matches(const std::string& tag, const std::string& want) {
    if (tag.size() == want.size()) return tag_prefix_equal(tag, want, want.size());
    return tag.size() > want.size() && tag[want.size()] == '.' && tag_prefix_equal(tag, want, want.size());
}

bool run_stats(const StatBundle& b, const StatInputs& in, const StatState& state, StatResult& out, std::string& err) {
    out = StatResult{};
    if (in.level < 1 || in.level > kMaxLevel) {
        err = "level " + std::to_string(in.level) + " is not 1 to " + std::to_string(kMaxLevel);
        return false;
    }
    const ClassDef* cls = b.class_by_enum(in.class_enum);
    if (!cls) {
        err = "no class " + std::to_string(in.class_enum) + " in the bundle";
        return false;
    }
    if (!check_effects(in.effects, err)) return false;
    const size_t ns = b.stats.size();
    std::vector<std::vector<uint32_t>> chains(ns);
    // An NPC profile (kNpcClassBase and more) holds a kind of NPC's whole chain of lists: the common lists are the players'.
    const bool npc = in.class_enum >= kNpcClassBase;
    if (!npc)
        for (const Step& s : b.common_steps) chains[s.stat].push_back(s.prog);
    for (const Step& s : cls->steps) chains[s.stat].push_back(s.prog);
    std::vector<double> mods(ns, 0.0);
    for (const StatMod& m : in.mods) {
        auto it = b.by_guid.find(m.stat);
        if (it == b.by_guid.end()) {
            ++out.ignored_mods;
            continue;
        }
        mods[it->second] += m.value;
    }
    // The effects' mods by stat, in the order given.
    std::vector<std::vector<EffectApp>> fx(ns);
    for (const ActiveEffect& e : in.effects) {
        if (e.stacks == 0) continue;
        for (const EffectMod& md : e.mods) {
            auto it = b.by_guid.find(md.stat);
            if (it == b.by_guid.end()) {
                ++out.ignored_effect_mods;
                continue;
            }
            fx[it->second].push_back({md.value, e.stacks});
        }
    }
    std::vector<bool> member(ns, false);
    if (!npc)
        for (uint32_t s : b.common_members) member[s] = true;
    for (uint32_t s : cls->members) member[s] = true;

    Machine m(b, in, state, ns);
    std::vector<double>& vals = m.vals();
    std::vector<double> init_v(ns, 0.0), base_v(ns, 0.0);
    std::vector<double> hi_v(ns, 0.0), lo_v(ns, 0.0);
    std::vector<bool> has_hi(ns, false), has_lo(ns, false);
    const bool single = b.mode == Mode::Single;
    for (uint32_t node : b.order) {
        const size_t s = node >> 1;
        const Stat& stat = b.stats[s];
        const bool mul = (stat.flags & kStatMulMods) != 0;
        if ((node & 1) == kNodeInit) {
            double v = mul ? 1.0 : 0.0;
            for (uint32_t p : chains[s]) {
                if (stat.base >= 0 && b.programs[p].reads_total) {
                    // The entry sees the stat's value so far, Base included, and its result is the stat's new value (plan E1, stage 3: the captured
                    // NPCs' MaxHealth, MaxMana and MoveSpeedMult are the elite scalar's `GetStatInitRunningTotal() * f` over initial + Base, 279 of
                    // 279; a player's `GetStatInitRunningTotal() + v` is the same either way). The initial value stored is what the value pass adds
                    // the Base to again. The same steps, in the same order, as the Go engine's (statprog/engine.go).
                    const double base = m.run(b.programs[static_cast<size_t>(stat.base)]);
                    if (mul) {
                        m.set_total(arith(op::Mul, base, v, single));
                        const double r = m.run(b.programs[p]);
                        if (base == 0) {
                            m.count_fault();
                            v = 0;
                        } else {
                            v = arith(op::Div, r, base, single);
                        }
                    } else {
                        m.set_total(arith(op::Add, v, base, single));
                        v = arith(op::Sub, m.run(b.programs[p]), base, single);
                    }
                    continue;
                }
                m.set_total(v);
                v = m.run(b.programs[p]);
            }
            m.set_total(0);
            init_v[s] = m.settle(v);
            continue;
        }
        double v = init_v[s];
        if (stat.base >= 0) {
            const double x = m.run(b.programs[static_cast<size_t>(stat.base)]);
            v = mul ? arith(op::Mul, x, v, single) : arith(op::Add, v, x, single);
        }
        base_v[s] = m.settle(v);
        v = arith(op::Add, base_v[s], mods[s], single);
        v = apply_effects(v, stat.flags, fx[s], single);
        double lo = std::numeric_limits<double>::quiet_NaN(), hi = lo;
        if (stat.min >= 0) {
            lo = m.run(b.programs[static_cast<size_t>(stat.min)]);
            if (lo > v) v = lo;
        }
        if (stat.max >= 0) {
            hi = m.run(b.programs[static_cast<size_t>(stat.max)]);
            if (hi < v) v = hi;
            hi_v[s] = hi;
            has_hi[s] = true;
        }
        if (stat.min >= 0) {
            lo_v[s] = lo;
            has_lo[s] = true;
        }
        if ((stat.flags & kStatSpawnMax) && stat.max >= 0) v = hi;
        if ((stat.flags & kStatSpawnMin) && stat.min >= 0) v = lo;
        vals[s] = m.settle(v);
    }
    for (const auto& [guid, value] : in.current) {
        const auto it = b.by_guid.find(guid);
        if (it != b.by_guid.end()) vals[it->second] = value;
    }
    for (size_t s = 0; s < ns; ++s) {
        if (!member[s]) continue;
        StatOut o;
        o.stat = static_cast<uint32_t>(s);
        o.guid = b.stats[s].guid;
        o.replication = b.stats[s].replication;
        o.flags = b.stats[s].flags;
        o.value = vals[s];
        o.base = base_v[s];
        o.equip = m.settle(mods[s]);   // a sum that is not finite, or beyond a float32, is a fault: a clamp can hide it from the value
        if (b.stats[s].regen >= 0) {
            o.regen = m.settle(m.run(b.programs[static_cast<size_t>(b.stats[s].regen)]));
            o.has_regen = true;
        }
        o.has_max = has_hi[s];
        o.max = has_hi[s] ? m.settle(hi_v[s]) : 0.0;
        o.has_min = has_lo[s];
        o.min = has_lo[s] ? m.settle(lo_v[s]) : 0.0;
        out.stats.push_back(o);
    }
    if (m.exhausted()) {
        out = StatResult{};
        err = "the run needs more than " + std::to_string(kMaxRunInstructions) + " instructions";
        return false;
    }
    out.faults = m.faults();
    out.instructions = m.used();
    out.values = vals;
    return true;
}

// ExprEvaluator: a Machine over the finished run's values.
struct ExprEvaluator::Impl {
    Impl(const StatBundle& b, const StatInputs& in, const StatState& st, const StatResult& r) : m(b, in, st, r.values.size()), bundle(b) {
        m.vals() = r.values;
    }
    Machine m;
    const StatBundle& bundle;
};

ExprEvaluator::ExprEvaluator(const StatBundle& bundle, const StatInputs& in, const StatState& state, const StatResult& result) {
    if (result.values.size() != bundle.stats.size()) {
        err_ = "the run has no stat values for this bundle";
        return;
    }
    if (in.level < 1 || in.level > kMaxLevel) {
        err_ = "level " + std::to_string(in.level) + " is not 1 to " + std::to_string(kMaxLevel);
        return;
    }
    if (!check_effects(in.effects, err_)) return;
    impl_ = new Impl(bundle, in, state, result);
}

ExprEvaluator::~ExprEvaluator() { delete impl_; }

bool ExprEvaluator::eval(int64_t prog, ExprHost& host, double& out, std::string& err) {
    out = 0;
    if (!impl_) {
        err = err_;
        return false;
    }
    if (!impl_->bundle.is_expr(prog)) {
        err = "program " + std::to_string(prog) + " is not an expression program";
        return false;
    }
    impl_->m.set_host(&host);
    impl_->m.reset_budget();
    impl_->m.use_actor(0);
    out = impl_->m.run(impl_->bundle.programs[static_cast<size_t>(prog)]);
    if (impl_->m.exhausted()) {
        out = 0;
        err = "the expression needs more than " + std::to_string(kMaxRunInstructions) + " instructions";
        return false;
    }
    return true;
}

int ExprEvaluator::faults() const { return impl_ ? impl_->m.faults() : 0; }

bool ExprEvaluator::set_actor(int role, const StatInputs& in, const StatState& state, const StatResult& result, std::string& err) {
    if (!impl_) {
        err = err_;
        return false;
    }
    if (role < 1 || role >= static_cast<int>(kMaxRoles)) {
        err = "no actor role " + std::to_string(role);
        return false;
    }
    if (result.values.size() != impl_->bundle.stats.size()) {
        err = "the run has no stat values for this bundle";
        return false;
    }
    if (!check_effects(in.effects, err)) return false;
    if (in.level < 1 || in.level > kMaxLevel) {
        err = "level " + std::to_string(in.level) + " is not 1 to " + std::to_string(kMaxLevel);
        return false;
    }
    ActorCtx c;
    c.vals = &result.values;
    c.level = static_cast<double>(in.level);
    c.state = &state;
    c.in = &in;
    impl_->m.set_actor(static_cast<size_t>(role), c);
    return true;
}

void ExprEvaluator::clear_actors() {
    if (impl_) impl_->m.clear_actors();
}

}  // namespace aoc::worldd::stats

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/stat_program.cpp: parse and verify a stat program bundle
//
//  The rules are those of services/internal/statprog (format.go, verify.go).
// ============================================================================
#include "stats/stat_program.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aoc::worldd::stats {

namespace {

constexpr size_t kHeaderSize = 16;
constexpr uint16_t kFlagSingle = 1;
constexpr uint16_t kFlagDouble = 2;

uint32_t crc32_ieee(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string num(uint64_t v) { return std::to_string(v); }
std::string snum(int64_t v) { return std::to_string(v); }

// Reader walks the body; the first failure sticks.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    const std::string& error() const { return err_; }
    size_t left() const { return n_ - off_; }
    size_t offset() const { return off_; }
// == Cyb | C.O.A ==

    void fail(const std::string& msg) {
        if (ok_) {
            ok_ = false;
            err_ = msg;
        }
    }
    bool need(size_t n) {
        if (!ok_) return false;
        if (n_ - off_ < n) {
            fail("truncated at byte " + num(kHeaderSize + off_));
            return false;
        }
        return true;
    }
    uint8_t u8() {
        if (!need(1)) return 0;
        return p_[off_++];
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = static_cast<uint16_t>(p_[off_] | (p_[off_ + 1] << 8));
        off_ += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = static_cast<uint32_t>(p_[off_]) | (static_cast<uint32_t>(p_[off_ + 1]) << 8) |
                     (static_cast<uint32_t>(p_[off_ + 2]) << 16) | (static_cast<uint32_t>(p_[off_ + 3]) << 24);
        off_ += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t lo = u32();
        uint64_t hi = u32();
        return lo | (hi << 32);
    }
    double f64() {
        uint64_t b = u64();
        double d;
        std::memcpy(&d, &b, 8);
        return d;
    }
    const uint8_t* take(size_t n) {
        if (!need(n)) return nullptr;
        const uint8_t* r = p_ + off_;
        off_ += n;
        return r;
    }
    // count reads a u32 count that must be at most max and whose items, each at least item_size
    // bytes, must fit in what is left.
    size_t count(const char* what, size_t max, size_t item_size) {
        uint32_t n = u32();
        if (!ok_) return 0;
        if (n > max) {
            fail(std::string(what) + ": " + num(n) + " is more than the limit " + num(max));
            return 0;
        }
        if (item_size > 0 && static_cast<uint64_t>(n) * item_size > left()) {
            fail(std::string(what) + ": " + num(n) + " items do not fit in the " + num(left()) + " bytes left");
            return 0;
        }
        return n;
    }

private:
    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
    std::string err_;
};

constexpr int operand_bytes(uint8_t o) {
    switch (o) {
        case op::Total: case op::Level: case op::Pop: case op::Add: case op::Sub: case op::Mul: case op::Div: case op::Mod:
        case op::Neg: case op::Not: case op::Lt: case op::Le: case op::Gt: case op::Ge: case op::Eq: case op::Ne:
        case op::Round: case op::Floor: case op::Ceil: case op::Min: case op::Max: case op::Sel: case op::Combat:
        case op::Moving: case op::Unkillable: case op::NpcRank: case op::Ret: case op::Lerp: case op::Abs: case op::Rand: case op::XpDebt: case op::HitTargets:
            return 0;
        case op::Host0: case op::Host1: case op::Host2:
            return 4;
        case op::Load: case op::Store: case op::Actor:
            return 1;
        case op::Const: case op::Stat: case op::Jmp: case op::Jz: case op::Jnz: case op::Curve: case op::Effect:
        case op::EffectTag: case op::Profession: case op::Consumed: case op::Progress: case op::EffectInst: case op::HitTag: case op::HitFlag:
        case op::NpcDist:
            return 2;
        case op::Skill: case op::EquipTag:
            return 4;
    }
    return -1;
}

// stack_effect: how many values an opcode pops and pushes.
void stack_effect(uint8_t o, int& pop, int& push) {
    switch (o) {
        case op::Const: case op::Load: case op::Stat: case op::Total: case op::Level: case op::Effect:
        case op::EffectTag: case op::Skill: case op::Combat: case op::Moving: case op::Unkillable:
        case op::EquipTag: case op::Profession: case op::Consumed: case op::NpcRank: case op::NpcDist: case op::Progress: case op::EffectInst: case op::Rand: case op::HitTag: case op::HitFlag: case op::XpDebt: case op::HitTargets:
            pop = 0, push = 1;
            return;
        case op::Host0:
            pop = 0, push = 1;
            return;
        case op::Host1:
            pop = 1, push = 1;
            return;
        case op::Host2:
            pop = 2, push = 1;
            return;
        case op::Store: case op::Pop: case op::Jz: case op::Jnz:
            pop = 1, push = 0;
            return;
        case op::Add: case op::Sub: case op::Mul: case op::Div: case op::Mod: case op::Lt: case op::Le: case op::Gt:
        case op::Ge: case op::Eq: case op::Ne: case op::Min: case op::Max:
            pop = 2, push = 1;
            return;
        case op::Neg: case op::Not: case op::Round: case op::Floor: case op::Ceil: case op::Curve: case op::Abs:
            pop = 1, push = 1;
            return;
        case op::Sel: case op::Lerp:
            pop = 3, push = 1;
            return;
    }
    pop = 0, push = 0;  // Jmp, Ret, Actor
}

struct ProgInfo {
    std::vector<uint16_t> stats;  // sorted, unique
    bool host = false;            // it uses a host operation (Progress, EffectInst, Rand): only an expression program may
    bool total = false;           // it reads the running total (op::Total): Program::reads_total
};

class Verifier {
public:
    Verifier(const StatBundle& b, std::string& err) : b_(b), err_(err) {}

    bool bad(const std::string& msg) {
        err_ = msg;
        return false;
    }
// (C:Y:B | CoA)

    bool analyze(size_t pi, ProgInfo& info) {
        const Program& p = b_.programs[pi];
        const std::vector<uint8_t>& code = p.code;
        const size_t n = code.size();
        const std::string pn = "program " + num(pi) + ": ";
        if (n < 1 || n > kMaxCodeBytes) return bad(pn + "code length " + num(n));
        if (p.locals > kMaxLocals) return bad(pn + num(p.locals) + " locals");
        std::vector<int> height(n + 1, -1);
        std::vector<bool> starts(n + 1, false);
        std::vector<size_t> targets;
        std::vector<bool> seen(b_.stats.size(), false);
        int cur = 0;
        bool reachable = true;
        size_t pc = 0;
        uint8_t last = 0;
        auto u16at = [&](size_t at) { return static_cast<size_t>(code[at] | (code[at + 1] << 8)); };
        while (pc < n) {
            const uint8_t o = code[pc];
            const int sz = operand_bytes(o);
            if (sz < 0) return bad(pn + "unknown opcode " + num(o) + " at " + num(pc));
            if (pc + 1 + static_cast<size_t>(sz) > n) return bad(pn + "opcode at " + num(pc) + " is cut off");
            starts[pc] = true;
            if (o == op::Progress || o == op::EffectInst || o == op::Rand || o == op::HitTag || o == op::HitFlag || o == op::XpDebt || o == op::HitTargets ||
                o == op::Host0 || o == op::Host1 || o == op::Host2 || o == op::Actor)
                info.host = true;
            if (o == op::Total) info.total = true;
            if (height[pc] >= 0) {
                if (reachable && cur != height[pc]) return bad(pn + "stack height falls into another at " + num(pc));
                cur = height[pc];
            } else if (!reachable) {
                return bad(pn + "unreachable code at " + num(pc));
            } else {
                height[pc] = cur;
            }
            reachable = true;
            auto below = [&](const char* what, size_t idx, size_t limit) {
                return bad(pn + what + " " + num(idx) + " at " + num(pc) + " is not below " + num(limit));
            };
            switch (o) {
                case op::Const:
                    if (u16at(pc + 1) >= b_.consts.size()) return below("constant", u16at(pc + 1), b_.consts.size());
                    break;
                case op::Load: case op::Store:
                    if (code[pc + 1] >= p.locals) return below("local", code[pc + 1], p.locals);
                    break;
                case op::Actor:
                    if (code[pc + 1] >= kMaxRoles) return below("actor role", code[pc + 1], kMaxRoles);
                    break;
                case op::NpcDist:
                    if (u16at(pc + 1) >= 8) return below("stat distribution index", u16at(pc + 1), 8);
                    break;
                case op::Stat: {
                    size_t s = u16at(pc + 1);
                    if (s >= b_.stats.size()) return below("stat", s, b_.stats.size());
                    if (!seen[s]) {
                        seen[s] = true;
                        info.stats.push_back(static_cast<uint16_t>(s));
                    }
                    break;
                }
                case op::Curve:
                    if (u16at(pc + 1) >= b_.curves.size()) return below("curve", u16at(pc + 1), b_.curves.size());
                    break;
                case op::Effect: case op::Consumed: case op::EffectInst:
                    if (u16at(pc + 1) >= b_.refs.size()) return below("reference", u16at(pc + 1), b_.refs.size());
                    break;
                case op::Skill:
                    for (size_t i = 0; i < 2; ++i)
                        if (u16at(pc + 1 + 2 * i) >= b_.refs.size()) return below("reference", u16at(pc + 1 + 2 * i), b_.refs.size());
                    break;
                case op::EffectTag: case op::Profession: case op::Progress: case op::HitTag: case op::HitFlag:
                    if (u16at(pc + 1) >= b_.strings.size()) return below("string", u16at(pc + 1), b_.strings.size());
                    break;
                case op::EquipTag:
                    for (size_t i = 0; i < 2; ++i)
                        if (u16at(pc + 1 + 2 * i) >= b_.strings.size()) return below("string", u16at(pc + 1 + 2 * i), b_.strings.size());
                    break;
                case op::Host0: case op::Host1: case op::Host2:
                    if (u16at(pc + 3) >= b_.strings.size()) return below("string", u16at(pc + 3), b_.strings.size());
                    if (u16at(pc + 1) < 1 || u16at(pc + 1) > 255) return below("host function", u16at(pc + 1), 256);
                    break;
                default:
                    break;
            }
            int pop = 0, push = 0;
            stack_effect(o, pop, push);
            if (o == op::Ret) {
                if (cur != 1) return bad(pn + "the stack holds " + num(static_cast<size_t>(cur)) + " values at the return (one is the result)");
                if (pc + 1 != n) return bad(pn + "code after the return at " + num(pc));
                reachable = false;
            } else if (o == op::Jmp || o == op::Jz || o == op::Jnz) {
                if (o != op::Jmp) {
                    if (cur < 1) return bad(pn + "stack underflow at " + num(pc));
                    --cur;
                }
                const size_t t = u16at(pc + 1);
                if (t < pc + 3 || t >= n) return bad(pn + "jump at " + num(pc) + " to " + num(t) + " (forward, inside the code)");
                if (height[t] >= 0 && height[t] != cur) return bad(pn + "jump at " + num(pc) + " arrives with another stack height");
                height[t] = cur;
                targets.push_back(t);
                if (o == op::Jmp) reachable = false;
            } else {
                if (cur < pop) return bad(pn + "stack underflow at " + num(pc));
                cur = cur - pop + push;
                if (static_cast<size_t>(cur) > kMaxStack) return bad(pn + "the stack is deeper than the limit at " + num(pc));
            }
            last = o;
            pc += 1 + static_cast<size_t>(sz);
        }
        if (last != op::Ret) return bad(pn + "does not end in a return");
        for (size_t t : targets)
            if (!starts[t]) return bad(pn + "a jump lands inside an instruction (offset " + num(t) + ")");
        std::sort(info.stats.begin(), info.stats.end());
        return true;
    }

    bool verify() {
        const size_t ns = b_.stats.size(), np = b_.programs.size();
        std::vector<ProgInfo> infos(np);
        for (size_t i = 0; i < np; ++i)
            if (!analyze(i, infos[i])) return false;
        std::unordered_map<int64_t, bool> guids;
        auto prog_ok = [&](int32_t p) { return p == -1 || (p >= 0 && static_cast<size_t>(p) < np); };
        for (size_t i = 0; i < ns; ++i) {
            const Stat& s = b_.stats[i];
            if (s.name >= b_.strings.size()) return bad("stat " + num(i) + ": name string is outside the strings");
            if (guids.count(s.guid)) return bad("stat " + num(i) + ": guid " + snum(s.guid) + " is a second stat's");
            if (s.replication > 7) return bad("stat " + num(i) + ": replication " + num(s.replication));
            const uint8_t allowed = kStatMulMods | kStatClampFinal | kStatSpawnMax | kStatSpawnMin | kStatBuckets | kStatFloatArray | kStatMulCompl;
            if (s.flags & ~allowed) return bad("stat " + num(i) + ": flags " + num(s.flags));
            if (!prog_ok(s.base) || !prog_ok(s.min) || !prog_ok(s.max) || !prog_ok(s.regen))
                return bad("stat " + num(i) + ": a program index is outside the programs");
            guids[s.guid] = true;
        }
        auto check_steps = [&](const char* what, const std::vector<Step>& steps) {
            for (size_t i = 0; i < steps.size(); ++i)
                if (steps[i].stat >= ns || steps[i].prog >= np)
                    return bad(std::string(what) + " " + num(i) + ": stat or program is outside the tables");
            return true;
        };
        auto check_members = [&](const char* what, const std::vector<uint32_t>& m) {
            for (size_t i = 0; i < m.size(); ++i)
                if (m[i] >= ns) return bad(std::string(what) + " " + num(i) + ": stat is outside the stats");
            return true;
        };
        if (!check_steps("common step", b_.common_steps) || !check_members("common member", b_.common_members)) return false;
        std::vector<bool> enums(256, false);
        for (size_t i = 0; i < b_.classes.size(); ++i) {
            const ClassDef& c = b_.classes[i];
            if (c.enum_value == 0 || enums[c.enum_value]) return bad("class " + num(i) + ": enum is 0 or a second class's");
            if (c.name >= b_.strings.size()) return bad("class " + num(i) + ": name string is outside the strings");
            enums[c.enum_value] = true;
            if (!check_steps("class step", c.steps) || !check_members("class member", c.members)) return false;
        }

        if (b_.order.size() != 2 * ns) return bad("the order has " + num(b_.order.size()) + " nodes, " + num(ns) + " stats need " + num(2 * ns));
        std::vector<int64_t> pos(2 * ns, -1);
        for (size_t i = 0; i < b_.order.size(); ++i) {
            const uint32_t node = b_.order[i];
            if ((node >> 1) >= ns) return bad("order " + num(i) + ": stat is outside the stats");
            if (pos[node] != -1) return bad("order " + num(i) + ": node appears twice");
            pos[node] = static_cast<int64_t>(i);
        }
        for (size_t s = 0; s < ns; ++s)
            if (pos[2 * s + kNodeInit] > pos[2 * s + kNodeValue]) return bad("stat " + num(s) + ": its value comes before its initial value");
        auto reads_before = [&](int32_t prog, size_t node, const std::string& what) {
            if (prog < 0) return true;
            for (uint16_t t : infos[static_cast<size_t>(prog)].stats)
                if (pos[2 * static_cast<size_t>(t) + kNodeValue] >= pos[node])
                    return bad(what + " reads stat " + num(t) + ", which is not settled before it");
            return true;
        };
        // A step that reads the running total sees the stat's value so far, Base included (run_stats): the Base runs at the initial-value node too,
        // so what it reads must be settled before that node.
        auto step_reads_before = [&](const Step& s, const std::string& what) {
            if (!reads_before(static_cast<int32_t>(s.prog), 2 * s.stat + kNodeInit, what)) return false;
            if (infos[s.prog].total && !reads_before(b_.stats[s.stat].base, 2 * s.stat + kNodeInit, "the Base of stat " + num(s.stat) + ", which " + what + " reads through the running total,"))
                return false;
            return true;
        };
        for (const Step& s : b_.common_steps)
            if (!step_reads_before(s, "a common initializer step of stat " + num(s.stat))) return false;
        for (const ClassDef& c : b_.classes)
            for (const Step& s : c.steps)
                if (!step_reads_before(s, "a class initializer step of stat " + num(s.stat))) return false;
        for (size_t i = 0; i < np; ++i) reads_total_[i] = infos[i].total;
        for (size_t i = 0; i < ns; ++i) {
            const Stat& s = b_.stats[i];
            for (int32_t p : {s.base, s.min, s.max})
                if (!reads_before(p, 2 * i + kNodeValue, "a formula of stat " + num(i))) return false;
        }
// (C O A + Cyb)

        // The expression programs: in range, strictly increasing; and no formula of the stat graph uses a host operation.
        for (size_t i = 0; i < b_.exprs.size(); ++i) {
            if (b_.exprs[i] >= np) return bad("expression " + num(i) + ": program " + num(b_.exprs[i]) + " is not below " + num(np));
            if (i > 0 && b_.exprs[i] <= b_.exprs[i - 1])
                return bad("expression " + num(i) + ": program " + num(b_.exprs[i]) + " does not come after program " + num(b_.exprs[i - 1]));
        }
        auto no_host = [&](int32_t prog, const std::string& what) {
            if (prog >= 0 && infos[static_cast<size_t>(prog)].host) return bad(what + " uses a host operation (only an expression program may)");
            return true;
        };
        for (const Step& s : b_.common_steps)
            if (!no_host(static_cast<int32_t>(s.prog), "a common initializer step of stat " + num(s.stat))) return false;
        for (const ClassDef& c : b_.classes)
            for (const Step& s : c.steps)
                if (!no_host(static_cast<int32_t>(s.prog), "a class initializer step of stat " + num(s.stat))) return false;
        for (size_t i = 0; i < ns; ++i) {
            const Stat& s = b_.stats[i];
            for (int32_t p : {s.base, s.min, s.max, s.regen})
                if (!no_host(p, "a formula of stat " + num(i))) return false;
        }
        return true;
    }

    /// What verify found per program: it reads the running total (set by verify; parse_bundle copies it into Program::reads_total).
    const std::vector<bool>& reads_total() const { return reads_total_; }

private:
    const StatBundle& b_;
    std::string& err_;
    std::vector<bool> reads_total_ = std::vector<bool>(b_.programs.size(), false);
};

}  // namespace

const ClassDef* StatBundle::class_by_enum(int e) const {
    for (const ClassDef& c : classes)
        if (c.enum_value == e) return &c;
    return nullptr;
}

bool StatBundle::is_expr(int64_t prog) const {
    return prog >= 0 && prog <= 0xFFFFFFFFll && std::binary_search(exprs.begin(), exprs.end(), static_cast<uint32_t>(prog));
}

bool parse_bundle(const uint8_t* data, size_t size, StatBundle& out, std::string& err) {
    out = StatBundle{};
    auto bad = [&](const std::string& m) {
        err = m;
        return false;
    };
    if (size < kHeaderSize) return bad(num(size) + " bytes is shorter than the 16-byte header");
    if (size > kMaxBundleBytes) return bad(num(size) + " bytes is more than the limit");
    if (std::memcmp(data, "AOSP", 4) != 0) return bad("magic");
    const uint16_t version = static_cast<uint16_t>(data[4] | (data[5] << 8));
    if (version != kBundleVersion) return bad("version " + num(version));
    const uint16_t flags = static_cast<uint16_t>(data[6] | (data[7] << 8));
    if ((flags & ~(kFlagSingle | kFlagDouble)) != 0 || flags == (kFlagSingle | kFlagDouble)) return bad("flags " + num(flags));
    const uint32_t body_len = static_cast<uint32_t>(data[8]) | (static_cast<uint32_t>(data[9]) << 8) |
                              (static_cast<uint32_t>(data[10]) << 16) | (static_cast<uint32_t>(data[11]) << 24);
    if (static_cast<uint64_t>(body_len) != size - kHeaderSize) return bad("body length " + num(body_len) + ", " + num(size - kHeaderSize) + " bytes follow the header");
    const uint8_t* body = data + kHeaderSize;
    const uint32_t crc = static_cast<uint32_t>(data[12]) | (static_cast<uint32_t>(data[13]) << 8) |
                         (static_cast<uint32_t>(data[14]) << 16) | (static_cast<uint32_t>(data[15]) << 24);
    if (crc != crc32_ieee(body, size - kHeaderSize)) return bad("crc32 does not match the body");
// (C O A) (C.Y.B)

    StatBundle& b = out;
    b.crc32 = crc;
    b.mode = flags == kFlagDouble ? Mode::Double : flags == kFlagSingle ? Mode::Single : Mode::Float32;
    Reader r(body, size - kHeaderSize);
    size_t n = r.count("constants", kMaxConsts, 8);
    b.consts.reserve(n);
    for (size_t i = 0; i < n && r.ok(); ++i) {
        double c = r.f64();
        if (r.ok() && !std::isfinite(c)) r.fail("constant " + num(i) + " is not finite");
        b.consts.push_back(c);
    }
    n = r.count("strings", kMaxStrings, 1);
    b.strings.reserve(n);
    for (size_t i = 0; i < n && r.ok(); ++i) {
        const size_t l = r.u8();
        const uint8_t* s = r.take(l);
        if (s) b.strings.emplace_back(reinterpret_cast<const char*>(s), l);
    }
    n = r.count("references", kMaxRefs, 16);
    b.refs.reserve(n);
    for (size_t i = 0; i < n && r.ok(); ++i) {
        RecordRef rr;
        rr.type_id = r.u64();
        rr.guid = static_cast<int64_t>(r.u64());
        b.refs.push_back(rr);
    }
    n = r.count("curves", kMaxCurves, 4);
    b.curves.reserve(n);
    for (size_t i = 0; i < n && r.ok(); ++i) {
        const size_t nk = r.count("curve keys", kMaxCurveKeys, 33);
        if (r.ok() && nk == 0) r.fail("curve " + num(i) + " has no keys");
        Curve c;
        c.keys.reserve(nk);
        for (size_t k = 0; k < nk && r.ok(); ++k) {
            CurveKey key;
            key.time = r.f64();
            key.value = r.f64();
            key.interp = r.u8();
            key.arrive = r.f64();
            key.leave = r.f64();
            if (!r.ok()) break;
            if (!std::isfinite(key.time) || !std::isfinite(key.value) || !std::isfinite(key.arrive) || !std::isfinite(key.leave)) r.fail("curve " + num(i) + " key " + num(k) + " is not finite");
            else if (key.interp > kInterpCubic) r.fail("curve " + num(i) + " key " + num(k) + " has interpolation " + num(key.interp));
            else if (k > 0 && !(key.time > c.keys[k - 1].time)) r.fail("curve " + num(i) + ": key " + num(k) + " is not after key " + num(k - 1));
            c.keys.push_back(key);
        }
        b.curves.push_back(std::move(c));
    }
    n = r.count("programs", kMaxPrograms, 6);
    b.programs.reserve(n);
    for (size_t i = 0; i < n && r.ok(); ++i) {
        Program p;
        p.locals = r.u16();
        const size_t cl = r.u32();
        if (r.ok() && (cl < 1 || cl > kMaxCodeBytes)) r.fail("program " + num(i) + ": code length " + num(cl));
        if (r.ok()) {
            const uint8_t* c = r.take(cl);
            if (c) p.code.assign(c, c + cl);
        }
        b.programs.push_back(std::move(p));
    }
    n = r.count("stats", kMaxStats, 30);
    b.stats.reserve(n);
    for (size_t i = 0; i < n && r.ok(); ++i) {
        Stat s;
        s.guid = static_cast<int64_t>(r.u64());
        s.name = r.u32();
        s.replication = r.u8();
        s.flags = r.u8();
        s.base = static_cast<int32_t>(r.u32());
        s.min = static_cast<int32_t>(r.u32());
        s.max = static_cast<int32_t>(r.u32());
        s.regen = static_cast<int32_t>(r.u32());
        b.stats.push_back(s);
    }
    auto get_steps = [&](const char* what) {
        std::vector<Step> out_steps;
        const size_t cnt = r.count(what, kMaxSteps, 8);
        out_steps.reserve(cnt);
        for (size_t i = 0; i < cnt && r.ok(); ++i) {
            Step s;
            s.stat = r.u32();
            s.prog = r.u32();
            out_steps.push_back(s);
        }
        return out_steps;
    };
    auto get_u32s = [&](const char* what, size_t max) {
        std::vector<uint32_t> v;
        const size_t cnt = r.count(what, max, 4);
        v.reserve(cnt);
        for (size_t i = 0; i < cnt && r.ok(); ++i) v.push_back(r.u32());
        return v;
    };
    b.common_steps = get_steps("common steps");
    b.common_members = get_u32s("common members", kMaxStats);
    n = r.count("classes", kMaxClasses, 13);
    b.classes.reserve(n);
    for (size_t i = 0; i < n && r.ok(); ++i) {
        ClassDef c;
        c.enum_value = r.u8();
        c.name = r.u32();
        c.steps = get_steps("class steps");
        c.members = get_u32s("class members", kMaxStats);
        b.classes.push_back(std::move(c));
    }
    b.order = get_u32s("order", 2 * kMaxStats);
    b.exprs = get_u32s("expression programs", kMaxExprs);
    if (!r.ok()) return bad(r.error());
    if (r.left() != 0) return bad(num(r.left()) + " bytes after the last table");

    Verifier v(b, err);
    if (!v.verify()) return false;
    for (size_t i = 0; i < b.programs.size(); ++i) b.programs[i].reads_total = v.reads_total()[i];
    b.by_guid.reserve(b.stats.size());
    for (size_t i = 0; i < b.stats.size(); ++i) b.by_guid[b.stats[i].guid] = static_cast<uint32_t>(i);
    return true;
}

}  // namespace aoc::worldd::stats

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/npc_ability_data.h
//
//  What a creature does when it attacks, from the game's own files (tools/npcabilities writes the three tables):
//    npc_abilities.tsv   archetype  ability  task  cooldown  stop_movement  order  min_distance  max_distance  max_angle  gates  unknown  fight  conds  why  starts  rotate
//                        the ability records the creature Blueprint's state tree uses, the seconds it waits before it uses one again, whether it stands
//                        still for it, and the conditions the tree puts on using it (the tree is tried in `order`; -1: no such condition); `fight` says the task is one of the
//                        attacks of the creature's fight, `conds` the other conditions (a roll, its health, the seconds into the fight, line of sight ...) and `why` what the table
//                        could not say (`unknown`) or "off" for a task a parameter of the tree switches off for the creature, and `starts` the other cooldowns the creature starts
//                        when it takes the task, and `rotate` ("speed:duration") the turning towards its target that runs with the ability
//    npc_montages.tsv    montage  guid  package  package_guid  length  rate_scale  sections  hits  root_motion  root_unreadable
//                        every animation montage beside the Blueprints: its object path, the ids the network layer knows it by, how long it
//                        plays at a play rate of 1, the hit notifies ("time:key") that tell when each of the ability's hits lands, and how far the
//                        montage carries the creature ("time:x:y:z": its root bone's travel, in cm ahead, to the right and up from where the creature
//                        stood when the montage began; empty for a montage that stays in place, or for one whose animation the tables could not read: `root_unreadable`)
//    npc_targeting.tsv   ability  key  distance  min_distance  angle  angle_center  cone_check  freeze  freeze_distance  freeze_scale  ignore_non_target  discovery  check_validity  los_check  distance_center  volumes  shapes
//                        where an ability's hit lands (see combat/bite_area.h): how the record finds its target (`discovery`), whether it measures anything at all (`check_validity`, with the
//                        cone's distance and angle behind `cone_check` and the line of sight behind `los_check`), and the volumes it sweeps (`volumes` of them, `shapes` places the ones that can be
//                        placed); and what the ability's root motion does when a body comes ahead of the creature (see RootMotionFreeze)
//
//  A creature whose Blueprint is in the first table plays its attacks with their own animation (combat/npc_ability_replicator.h); one that is not
//  keeps the single swing it always had.  Header only: both loaders are plain text.
// ============================================================================
#pragma once

#include "combat/bite_area.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace aoc::worldd::combat {

/// One condition of a task, as the creature table says it (tools/npcabilities writes these as text; see its Conditions.cs for the grammar).
struct CondAtom {
    enum class Kind : uint8_t { True, False, In, Out, CentreIn, CentreOut, Angle, Cooldown, Random, HealthUnder, HealthNotUnder, FightTime, Sees, NotSees };
    enum Op : int { Eq, Ne, Lt, Le, Gt, Ge };
    Kind        kind = Kind::True;
    double      value = 0;   // the distance (cm), angle (degrees), probability, health fraction or seconds
    int         op = Eq;     // FightTime: how the seconds compare with `value`
    std::string name;        // Cooldown: the cooldown's name
};

/// A condition with how it joins the ones before it: `operand` 'c' (the first of a list), 'a' (and) or 'o' (or), and `delta` the brackets it opens before itself (> 0) or closes after itself (< 0).
struct CondToken {
    char     operand = 'c';
    int      delta = 0;
    CondAtom atom;
};
using CondGroup = std::vector<CondToken>;

/// What the conditions are asked about: the creature, its fight and its target as they are when the tree is tried.
struct CondContext {
    bool   preview = false;             // the creature only stands ready (no target, no roll): everything that changes during a fight counts as holding
    bool   has_target = false;
    double surface_distance = 0;        // from the creature's surface to the target's
    double centre_distance = 0;         // between the two centres, in 3D
    double angle = 0;                   // between the way the creature faces and the way to the target, 0 to 180 degrees
    std::function<bool()> sees;         // whether the creature sees its target (asked only by a condition that needs it)
    double health_fraction = 1;         // the creature's own health over its maximum
    double fight_seconds = 0;           // since its fight began
    std::function<bool(const std::string&)> cooldown_over;
    std::function<double()> roll;       // a fresh number in [0, 1)
};

/// Whether one condition holds.
inline bool eval_atom(const CondAtom& a, const CondContext& c) {
    using K = CondAtom::Kind;
    switch (a.kind) {
        case K::True: return true;
        case K::False: return false;
        case K::In: return !c.has_target || c.surface_distance <= a.value;
        case K::Out: return !c.has_target || c.surface_distance >= a.value;
        case K::CentreIn: return !c.has_target || c.centre_distance < a.value;
        case K::CentreOut: return !c.has_target || !(c.centre_distance < a.value);
        case K::Angle: return !c.has_target || c.angle <= a.value;
        case K::Cooldown: return !c.cooldown_over || c.cooldown_over(a.name);
        case K::Random: return c.preview || !c.roll || c.roll() < a.value;
        case K::HealthUnder: return c.preview || c.health_fraction < a.value;
        case K::HealthNotUnder: return c.preview || !(c.health_fraction < a.value);
        case K::FightTime: {
            if (c.preview) return true;
            const double x = c.fight_seconds;
            switch (a.op) {
                case CondAtom::Eq: return std::abs(x - a.value) < 1e-9;
                case CondAtom::Ne: return std::abs(x - a.value) >= 1e-9;
                case CondAtom::Lt: return x < a.value;
                case CondAtom::Le: return x <= a.value;
                case CondAtom::Gt: return x > a.value;
                case CondAtom::Ge: return x >= a.value;
            }
            return false;
        }
        case K::Sees: return !c.has_target || !c.sees || c.sees();
        case K::NotSees: return !c.has_target || (c.sees && !c.sees());
    }
    return false;
}

/// Whether a state's conditions hold, joined the way the tree joins them: each condition is merged into what came before with its operand, and a positive indent opens that many
/// brackets in front of it, a negative one closes that many behind it ("A and (B or C)" is A, B + 1 bracket with `and`, C - 1 with `or`).
inline bool eval_group(const CondGroup& group, const CondContext& c) {
    struct Frame {
        bool value = false;
        bool has = false;
        char operand = 'c';   // how this frame's value joins the one below it when it closes
    };
    std::vector<Frame> stack(1);
    for (size_t i = 0; i < group.size(); ++i) {
        const CondToken& t = group[i];
        const char operand = i == 0 ? 'c' : t.operand;
        for (int k = 0; k < t.delta; ++k) {
            Frame f;
            f.operand = k == 0 ? operand : 'c';
            stack.push_back(f);
        }
        const bool v = eval_atom(t.atom, c);
        Frame& top = stack.back();
        const char use = t.delta > 0 ? 'c' : operand;
        if (!top.has || use == 'c') top.value = v;
        else if (use == 'a') top.value = top.value && v;
        else top.value = top.value || v;
        top.has = true;
        for (int k = 0; k < -t.delta && stack.size() > 1; ++k) {
            const Frame closed = stack.back();
            stack.pop_back();
            Frame& below = stack.back();
            if (!below.has || closed.operand == 'c') below.value = closed.value;
            else if (closed.operand == 'a') below.value = below.value && closed.value;
            else below.value = below.value || closed.value;
            below.has = true;
        }
    }
    while (stack.size() > 1) {   // (brackets the tree left open close at its end)
        const Frame closed = stack.back();
        stack.pop_back();
        Frame& below = stack.back();
        if (!below.has || closed.operand == 'c') below.value = closed.value;
        else if (closed.operand == 'a') below.value = below.value && closed.value;
        else below.value = below.value || closed.value;
        below.has = true;
    }
    return stack.front().has ? stack.front().value : true;
}

/// One use-ability task of a creature Blueprint's state tree.
struct CreatureAbility {
    int64_t     ability = 0;       // the ability record's guid
    std::string task;              // the name its cooldown goes by
    double      cooldown = 0;      // seconds the creature waits before it uses the ability again
    bool        stop_movement = false;   // the creature stands still while the ability plays
    int         order = 0;         // the task's place in the tree: the creature tries the tasks in this order and uses the first whose conditions hold
    double      min_distance = -1, max_distance = -1, max_angle = -1;   // the target must be farther than / nearer than / within this angle of the facing (cm, degrees; -1: no condition)
    std::vector<std::string> gates;   // cooldown names that must be over
    bool        unknown = false;   // the tree puts a condition on the task that the table does not say
    bool        fight = true;      // the task is one of the creature's attacks of the fight (not an opening, idle or ambient task)
    bool        off = false;       // a parameter of the tree switches the task off for this creature
    std::string why;               // what the table does not say, when `unknown`
    std::vector<std::pair<std::string, double>> starts;   // the other cooldowns (name, seconds) the creature starts when it takes the task: the tasks of the states above it start them
    bool        rotates = false;       // the creature keeps turning towards its target while the ability plays
    double      rotate_speed = -1;     // ... at this many degrees a second (<= 0: its own rate) ...
    double      rotate_seconds = 0;    // ... for this long (0: until the ability is over)
    std::vector<CondGroup> groups; // the other conditions, one group per state of the task's way down: every group has to hold
};

/// What an ability's root motion does when a body comes ahead of the creature (the ability record's RootMotionFreeze fields): the creature's body, `radius_scale` times as wide, is looked at
/// `distance` cm ahead of it and as wide as itself; a body (the ability's target, or any player unless `ignore_non_target`) in that box stops the lunge for good (Stop) or while it stays there
/// (Pause).
struct RootMotionFreezeRule {
    enum class Kind : uint8_t { None, Pause, Stop };
    Kind   kind = Kind::None;
    double distance = 0;
    double radius_scale = 1;
    bool   ignore_non_target = true;
};

/// One animation montage.
struct MontageData {
    struct Hit {
        double time = 0;           // seconds into the montage at a play rate of 1
        int    key = 0;            // which hit of the ability it fires (the ability's hit rows carry the same key)
    };
    std::string      path;         // "/Game/.../Name.Name", as the ability record names it
    std::string      package;
    uint64_t         guid = 0, package_guid = 0;
    double           length = 0;   // seconds at a play rate of 1
    double           rate_scale = 1;
    bool             root_unreadable = false;   // the animation moves the creature in a form the tables could not read: root_motion is empty then and does not mean "stays in place"
    std::vector<Hit> hits;         // in time order
    /// How far the montage carries the creature: where its root bone is (cm ahead and to the right of where the creature faces, from where it stood when the montage began) at each
    /// time (seconds into the montage at a play rate of 1), in time order from 0.  Empty: the montage stays in place.
    struct Step {
        double time = 0, x = 0, y = 0;
    };
    std::vector<Step> root_motion;

    /// Where the root motion is `at` seconds into the montage (a play rate of 1): linear between the steps, the last one after the end.
    Step root_at(double at) const {
        Step s;
        s.time = at;
        if (root_motion.empty()) return s;
        if (at <= root_motion.front().time) {
            s.x = root_motion.front().x;
            s.y = root_motion.front().y;
            return s;
        }
        for (size_t i = 1; i < root_motion.size(); ++i) {
            const Step& a = root_motion[i - 1];
            const Step& b = root_motion[i];
            if (at > b.time) continue;
            const double span = b.time - a.time;
            const double f = span > 0 ? (at - a.time) / span : 1.0;
            s.x = a.x + (b.x - a.x) * f;
            s.y = a.y + (b.y - a.y) * f;
            return s;
        }
        s.x = root_motion.back().x;
        s.y = root_motion.back().y;
        return s;
    }

    /// Seconds the montage plays for at `play_rate`.
    double seconds(double play_rate) const {
        const double rate = rate_scale * play_rate;
        return rate > 0 ? length / rate : length;
    }
};

class NpcAbilityData {
public:
    /// Reads the three tables; false, with `error`, when one is missing or malformed.  Nothing changes on a failure.
    bool load(const std::string& abilities_path, const std::string& montages_path, const std::string& targeting_path, std::string& error) {
        std::map<std::string, std::vector<CreatureAbility>> abilities;
        std::map<std::string, MontageData>                  montages;
        std::map<int64_t, std::map<int, BiteArea>>          areas;
        std::vector<std::vector<std::string>>               rows;
        if (!read_table(abilities_path, "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate", rows, error)) return false;
        for (const auto& r : rows) {
            CreatureAbility a;
            if (!number(r[1], a.ability) || !number(r[3], a.cooldown) || !number(r[5], a.order) || !number(r[6], a.min_distance) || !number(r[7], a.max_distance) || !number(r[8], a.max_angle)) {
                return bad(abilities_path, r[0], error);
            }
            a.task = r[2];
            a.stop_movement = r[4] == "1";
            for (std::string& g : split(r[9], ';')) {
                if (!g.empty()) a.gates.push_back(std::move(g));
            }
            a.unknown = r[10] == "1";
            a.fight = r[11] == "1";
            a.why = r[13];
            for (const std::string& st : split(r[14], ';')) {
                if (st.empty()) continue;
                const size_t eq = st.find('=');
                double seconds = 0;
                if (eq == std::string::npos || !number(st.substr(eq + 1), seconds)) return bad(abilities_path, r[0], error);
                a.starts.emplace_back(st.substr(0, eq), seconds);
            }
            if (!r[15].empty()) {   // "speed:duration"
                const size_t colon = r[15].find(':');
                if (colon == std::string::npos || !number(r[15].substr(0, colon), a.rotate_speed) || !number(r[15].substr(colon + 1), a.rotate_seconds)) return bad(abilities_path, r[0], error);
                a.rotates = true;
            }
            a.off = !a.unknown && a.why == "off";
            if (!parse_conditions(r[12], a.groups)) return bad(abilities_path, r[0], error);
            abilities[r[0]].push_back(std::move(a));
        }
        for (auto& [archetype, tasks] : abilities) {
            std::stable_sort(tasks.begin(), tasks.end(), [](const CreatureAbility& x, const CreatureAbility& y) { return x.order < y.order; });
        }
        if (!read_table(montages_path, "montage\tguid\tpackage\tpackage_guid\tlength\trate_scale\tsections\thits\troot_motion\troot_unreadable", rows, error)) return false;
        for (const auto& r : rows) {
            MontageData m;
            m.path = r[0];
            m.package = r[2];
            m.root_unreadable = r[9] == "1";
            if (!number(r[1], m.guid) || !number(r[3], m.package_guid) || !number(r[4], m.length) || !number(r[5], m.rate_scale)) return bad(montages_path, r[0], error);
            for (const std::string& h : split(r[7], ';')) {
                if (h.empty()) continue;
                const size_t colon = h.find(':');
                MontageData::Hit hit;
                if (colon == std::string::npos || !number(h.substr(0, colon), hit.time) || !number(h.substr(colon + 1), hit.key)) return bad(montages_path, r[0], error);
                m.hits.push_back(hit);
            }
            for (const std::string& s : split(r[8], ';')) {
                if (s.empty()) continue;
                const std::vector<std::string> f = split(s, ':');
                MontageData::Step step;
                double up = 0;
                if (f.size() != 4 || !number(f[0], step.time) || !number(f[1], step.x) || !number(f[2], step.y) || !number(f[3], up)) return bad(montages_path, r[0], error);
                if (!m.root_motion.empty() && step.time < m.root_motion.back().time) return bad(montages_path, r[0], error);
                m.root_motion.push_back(step);
            }
            montages[m.path] = std::move(m);
        }
        if (!read_table(targeting_path,
                        "ability\tkey\tdistance\tmin_distance\tangle\tangle_center\tcone_check\tfreeze\tfreeze_distance\tfreeze_scale\tignore_non_target\tdiscovery\tcheck_validity\tlos_check\tdistance_center\tvolumes\tshapes",
                        rows, error)) {
            return false;
        }
        std::map<int64_t, RootMotionFreezeRule> freezes;
        for (const auto& r : rows) {
            int64_t ability = 0;
            int key = 0;
            BiteArea area;
            RootMotionFreezeRule rule;
            if (!number(r[0], ability) || !number(r[1], key) || !number(r[2], area.distance) || !number(r[3], area.min_distance) || !number(r[4], area.angle) || !number(r[5], area.angle_center) ||
                !number(r[8], rule.distance) || !number(r[9], rule.radius_scale) || (r[7] != "none" && r[7] != "pause" && r[7] != "stop") || !number(r[14], area.distance_center) ||
                !number(r[15], area.volume_count)) {
                return bad(targeting_path, r[0], error);
            }
            area.cone_check = r[6] == "1";
            rule.kind = r[7] == "stop" ? RootMotionFreezeRule::Kind::Stop : r[7] == "pause" ? RootMotionFreezeRule::Kind::Pause : RootMotionFreezeRule::Kind::None;
            rule.ignore_non_target = r[10] == "1";
            static const std::map<std::string, BiteArea::Discovery> discoveries = {{"none", BiteArea::Discovery::None},
                                                                                   {"prefers_active", BiteArea::Discovery::PrefersActive},
                                                                                   {"only_locked", BiteArea::Discovery::OnlyLocked},
                                                                                   {"only_active", BiteArea::Discovery::OnlyActive},
                                                                                   {"prefers_locked", BiteArea::Discovery::PrefersLocked}};
            const auto found = discoveries.find(r[11]);
            if (found == discoveries.end() || (r[12] != "0" && r[12] != "1") || (r[13] != "0" && r[13] != "1")) return bad(targeting_path, r[0], error);
            area.discovery = found->second;
            area.check_validity = r[12] == "1";
            area.los_check = r[13] == "1";
            // the volumes the sweep can place ("sphere:ox:oy:oz:radius:los", "box:ox:oy:oz:hx:hy:hz:yaw:los"), and the ones it cannot ("other:why")
            size_t entries = 0;
            for (const std::string& s : split(r[16], ';')) {
                if (s.empty()) continue;
                ++entries;
                const std::vector<std::string> f = split(s, ':');
                HitVolume v;
                if (f[0] == "sphere" && f.size() == 6 && number(f[1], v.ox) && number(f[2], v.oy) && number(f[3], v.oz) && number(f[4], v.radius) && (f[5] == "0" || f[5] == "1")) {
                    v.shape = HitVolume::Shape::Sphere;
                    v.los = f[5] == "1";
                    area.volumes.push_back(v);
                } else if (f[0] == "box" && f.size() == 9 && number(f[1], v.ox) && number(f[2], v.oy) && number(f[3], v.oz) && number(f[4], v.hx) && number(f[5], v.hy) && number(f[6], v.hz) &&
                           number(f[7], v.yaw) && (f[8] == "0" || f[8] == "1")) {
                    v.shape = HitVolume::Shape::Box;
                    v.los = f[8] == "1";
                    area.volumes.push_back(v);
                } else if (f[0] == "other" && f.size() == 2) {
                    area.readable = false;
                    area.unreadable_why = f[1];
                } else {
                    return bad(targeting_path, r[0], error);
                }
            }
            if (entries != area.volume_count) return bad(targeting_path, r[0], error);   // (the count and the shapes agree)
            areas[ability][key] = area;
            freezes[ability] = rule;   // (the same on every line of the ability)
        }
        abilities_ = std::move(abilities);
        montages_ = std::move(montages);
        areas_ = std::move(areas);
        freezes_ = std::move(freezes);
        return true;
    }

    bool   empty() const { return abilities_.empty(); }
    /// Every Blueprint's use-ability tasks, by archetype path.
    const std::map<std::string, std::vector<CreatureAbility>>& by_kind() const { return abilities_; }
    size_t kinds() const { return abilities_.size(); }
    size_t montages() const { return montages_.size(); }

    /// The use-ability tasks of the Blueprint with the archetype path ("/Game/.../Rabid.Default__Rabid_C"); null when the tables have none.
    const std::vector<CreatureAbility>* abilities_of(const std::string& archetype) const {
        const auto it = abilities_.find(archetype);
        return it == abilities_.end() ? nullptr : &it->second;
    }
    /// Where the hit of the ability that carries `hit_key` lands: the targeting record with that key, else the ability's own (key 0), else its first; null when the tables have no record for it.
    const BiteArea* area_of(int64_t ability, int hit_key = 0) const {
        const auto it = areas_.find(ability);
        if (it == areas_.end() || it->second.empty()) return nullptr;
        for (const int k : {hit_key, 0}) {
            const auto row = it->second.find(k);
            if (row != it->second.end()) return &row->second;
        }
        return &it->second.begin()->second;
    }
    /// Every targeting record the tables hold, by ability and key (for the census and the tests).
    const std::map<int64_t, std::map<int, BiteArea>>& areas() const { return areas_; }
    /// What the ability's root motion does when a body comes ahead of the creature; null when the tables have no line of the ability.
    const RootMotionFreezeRule* freeze_of(int64_t ability) const {
        const auto it = freezes_.find(ability);
        return it == freezes_.end() ? nullptr : &it->second;
    }
    /// The montage with the object path an ability record names; null when the tables do not have it.
    const MontageData* montage(const std::string& path) const {
        const auto it = montages_.find(path);
        return it == montages_.end() ? nullptr : &it->second;
    }

    /// What the tables say about each creature Blueprint's tasks (by the Blueprint's archetype path): how many a fight can use, how many a parameter of its tree switches off, how many belong
    /// to its opening or idle states, and how many are left out because a condition is not said (counted by condition).
    struct Coverage {
        size_t tasks = 0, usable = 0, off = 0, outside_fight = 0, skipped = 0;
        std::map<std::string, size_t> reasons;
    };
    std::map<std::string, Coverage> coverage() const {
        std::map<std::string, Coverage> out;
        for (const auto& [archetype, tasks] : abilities_) {
            Coverage& c = out[archetype];
            for (const CreatureAbility& t : tasks) {
                ++c.tasks;
                if (!t.fight) {
                    ++c.outside_fight;
                } else if (t.unknown) {
                    ++c.skipped;
                    for (const std::string& why : split(t.why, ',')) {
                        if (!why.empty()) ++c.reasons[why];
                    }
                } else if (t.off) {
                    ++c.off;
                } else {
                    ++c.usable;
                }
            }
        }
        return out;
    }

    /// One line per Blueprint with what coverage() counts: "Wolf: 1 of 8 tasks usable in the fight; 6 switched off by the tree's parameters, 0 outside the fight, 1 skipped; skipped for: ...".
    std::vector<std::string> coverage_lines() const {
        std::vector<std::string> lines;
        for (const auto& [archetype, c] : coverage()) {
            const size_t slash = archetype.find_last_of('/');
            const size_t from = slash == std::string::npos ? 0 : slash + 1;
            const size_t dot = archetype.find('.', from);
            std::vector<std::pair<std::string, size_t>> reasons(c.reasons.begin(), c.reasons.end());
            std::stable_sort(reasons.begin(), reasons.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string line = archetype.substr(from, dot == std::string::npos ? std::string::npos : dot - from) + ": " + std::to_string(c.usable) + " of " + std::to_string(c.tasks) +
                               " tasks usable in the fight; " + std::to_string(c.off) + " switched off by the tree's parameters, " + std::to_string(c.outside_fight) + " outside the fight, " +
                               std::to_string(c.skipped) + " skipped";
            for (size_t i = 0; i < reasons.size() && i < 3; ++i) line += (i ? ", " : "; skipped for: ") + reasons[i].first + " x" + std::to_string(reasons[i].second);
            lines.push_back(std::move(line));
        }
        return lines;
    }

private:
    static std::string percent_decoded(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                int v = 0;
                const auto r = std::from_chars(s.data() + i + 1, s.data() + i + 3, v, 16);
                if (r.ec == std::errc() && r.ptr == s.data() + i + 3) {
                    out.push_back(static_cast<char>(v));
                    i += 2;
                    continue;
                }
            }
            out.push_back(s[i]);
        }
        return out;
    }
    /// The conditions column of a task: groups joined by '|', in each the conditions joined by ' ' as `<c|a|o><indent>:<atom>`.  False for text that does not read.
    static bool parse_conditions(const std::string& text, std::vector<CondGroup>& out) {
        out.clear();
        for (const std::string& g : split(text, '|')) {
            if (g.empty()) continue;
            CondGroup group;
            for (const std::string& token : split(g, ' ')) {
                if (token.empty()) continue;
                const size_t colon = token.find(':');
                if (colon == std::string::npos || colon < 2) return false;
                CondToken t;
                t.operand = token[0];
                if (t.operand != 'c' && t.operand != 'a' && t.operand != 'o') return false;
                std::string delta = token.substr(1, colon - 1);
                if (!delta.empty() && delta[0] == '+') delta.erase(0, 1);
                if (!number(delta, t.delta)) return false;
                const std::string atom = token.substr(colon + 1);
                if (atom == "T") t.atom.kind = CondAtom::Kind::True;
                else if (atom == "F") t.atom.kind = CondAtom::Kind::False;
                else if (atom == "los") t.atom.kind = CondAtom::Kind::Sees;
                else if (atom == "nlos") t.atom.kind = CondAtom::Kind::NotSees;
                else if (atom[0] == '?') t.atom.kind = CondAtom::Kind::False;   // (a condition the table does not say: its task is `unknown` and never asked)
                else {
                    const size_t open = atom.find('(');
                    if (open == std::string::npos || atom.back() != ')') return false;
                    const std::string name = atom.substr(0, open);
                    const std::vector<std::string> args = split(atom.substr(open + 1, atom.size() - open - 2), ',');
                    using K = CondAtom::Kind;
                    if (name == "cd") {
                        if (args.size() != 1) return false;
                        t.atom.kind = K::Cooldown;
                        t.atom.name = percent_decoded(args[0]);
                    } else if (name == "fc") {
                        if (args.size() != 2 || !number(args[1], t.atom.value)) return false;
                        static const std::map<std::string, int> ops = {{"eq", CondAtom::Eq}, {"ne", CondAtom::Ne}, {"lt", CondAtom::Lt}, {"le", CondAtom::Le}, {"gt", CondAtom::Gt}, {"ge", CondAtom::Ge}};
                        const auto it = ops.find(args[0]);
                        if (it == ops.end()) return false;
                        t.atom.kind = K::FightTime;
                        t.atom.op = it->second;
                    } else {
                        static const std::map<std::string, K> by_name = {{"in", K::In},   {"out", K::Out},   {"cin", K::CentreIn},          {"cout", K::CentreOut},
                                                                       {"ang", K::Angle}, {"rand", K::Random}, {"hp", K::HealthUnder}, {"nhp", K::HealthNotUnder}};
                        const auto it = by_name.find(name);
                        if (it == by_name.end() || args.size() != 1 || !number(args[0], t.atom.value)) return false;
                        t.atom.kind = it->second;
                    }
                }
                group.push_back(std::move(t));
            }
            if (!group.empty()) out.push_back(std::move(group));
        }
        return true;
    }
    static bool bad(const std::string& file, const std::string& row, std::string& error) {
        error = file + ": a line of " + row + " does not read";
        return false;
    }
    static std::vector<std::string> split(const std::string& s, char sep) {
        std::vector<std::string> out;
        size_t from = 0;
        for (;;) {
            const size_t at = s.find(sep, from);
            if (at == std::string::npos) {
                out.push_back(s.substr(from));
                return out;
            }
            out.push_back(s.substr(from, at - from));
            from = at + 1;
        }
    }
    template <typename T>
    static bool number(const std::string& s, T& out) {
        if (s.empty()) return false;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
        return r.ec == std::errc() && r.ptr == s.data() + s.size();
    }
    static bool read_table(const std::string& path, std::string_view header, std::vector<std::vector<std::string>>& rows, std::string& error) {
        rows.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = path + " cannot be opened";
            return false;
        }
        const size_t columns = split(std::string(header), '\t').size();
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (first) {
                first = false;
                if (line != header) {
                    error = path + ": the header is not the one this build reads";
                    return false;
                }
                continue;
            }
            if (line.empty()) continue;
            std::vector<std::string> cells = split(line, '\t');
            if (cells.size() != columns) {
                error = path + ": a line has " + std::to_string(cells.size()) + " columns, not " + std::to_string(columns);
                return false;
            }
            rows.push_back(std::move(cells));
        }
        if (first) {
            error = path + " is empty";
            return false;
        }
        return true;
    }

    std::map<std::string, std::vector<CreatureAbility>> abilities_;
    std::map<std::string, MontageData>                  montages_;
    std::map<int64_t, std::map<int, BiteArea>>          areas_;
    std::map<int64_t, RootMotionFreezeRule>             freezes_;
};

}  // namespace aoc::worldd::combat

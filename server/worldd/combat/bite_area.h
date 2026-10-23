// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/bite_area.h
//
//  Where a creature's hit lands, from the ability's targeting record (tools/npcabilities writes it into npc_targeting.tsv).  A record has two parts and a hit lands when either holds:
//
//    the target part   (the record names how the target is found: the creature's own target, or the one the cast was aimed at)
//        validity off  the hit lands on that target wherever it stands: no distance, no angle, no line of sight.  The reach such a record states (its valid distance and angle) is not
//                      tested.  This is the rule of the wolf's and the bear's bites and swings.
//        validity on   the hit lands only while the target is inside the record's cone: its body no farther than the valid distance from the point `distance_center` ahead of the creature
//                      (along the facing turned by `angle_center`), no nearer than the minimum, and within half the valid angle either side of that axis (all round when the angle is 0 or
//                      360); a line of sight to it when the record asks.  The cone and the line of sight are separate switches of the record.
//    the volumes       every enemy whose body overlaps one of the record's shapes lands the hit, placed ahead of (x) and to the right of (y) the creature and above its feet (z).
//
//  A volume or origin this file cannot place makes the record `readable = false`: the world does not animate the ability then (the creature keeps its single swing).
//  Header only; the loader is in combat/npc_ability_data.h.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aoc::worldd::combat {

/// One volume of a targeting record that the sweep can place: a sphere or a box, in the creature's frame.
struct HitVolume {
    enum class Shape : uint8_t { Sphere, Box };
    Shape  shape = Shape::Sphere;
    double ox = 0, oy = 0, oz = 0;   // cm: ahead of the creature, to its right, above its feet
    double radius = 0;               // a sphere
    double hx = 0, hy = 0, hz = 0;   // a box: half its size along the creature's facing, across it and in height
    double yaw = 0;                  // degrees: how far a box is turned from the creature's facing
    bool   los = true;               // the target has to be seen from the creature
};

/// The targeting record of an ability's hit, as far as a creature's attack needs it.
struct BiteArea {
    enum class Discovery : uint8_t { None, PrefersActive, OnlyLocked, OnlyActive, PrefersLocked };
    enum class Gate : uint8_t { None, Any, Validity, Volumes };

    Discovery discovery = Discovery::None;   // how the record finds its target (None: it has no target part)
    bool      check_validity = false;        // the cone and the line of sight below count
    bool      cone_check = false;            // ... the distance and the angle count
    bool      los_check = false;             // ... the line of sight counts
    double    distance = 0, min_distance = 0, angle = 0, angle_center = 0, distance_center = 0;   // cm, degrees
    std::vector<HitVolume> volumes;
    size_t    volume_count = 0;              // the volumes the record has, the unreadable ones too
    bool      readable = true;               // false: a volume (or the origin of one) the sweep cannot place
    std::string unreadable_why;              // what the sweep cannot place, when it is not `readable`

    bool has_target_part() const { return discovery != Discovery::None; }
    /// The kind of rule the record makes: the target part alone with nothing to measure (Any), the target part with its cone or line of sight (Validity), or volumes to sweep (Volumes).
    Gate gate() const {
        if (volume_count > 0) return Gate::Volumes;
        if (!has_target_part()) return Gate::None;
        return check_validity && (cone_check || los_check) ? Gate::Validity : Gate::Any;
    }
    /// Whether the world can decide a hit by this record at all.
    bool usable() const { return readable && (has_target_part() || !volumes.empty()); }
};

/// The creature and one body near it at the moment a hit lands.
struct HitSubject {
    double cx = 0, cy = 0, yaw = 0;   // the creature's centre and facing (degrees, counted as the world counts a yaw)
    double tx = 0, ty = 0;            // the body's centre
    double tradius = 0;               // ... and how wide it is
};

namespace bite_area_detail {
constexpr double kPi = 3.14159265358979323846;
inline double wrap(double a) {
    while (a > 180.0) a -= 360.0;
    while (a <= -180.0) a += 360.0;
    return a;
}
}  // namespace bite_area_detail

/// Whether the body is inside the cone of a record whose validity is on and whose cone switch is on.
inline bool validity_holds(const BiteArea& a, const HitSubject& s) {
    using namespace bite_area_detail;
    if (a.distance <= 0) return false;   // a cone with no reach holds nothing
    const double axis = s.yaw + a.angle_center;
    const double rad = axis * kPi / 180.0;
    const double ax = s.cx + std::cos(rad) * a.distance_center, ay = s.cy + std::sin(rad) * a.distance_center;   // where the cone starts
    const double dx = s.tx - ax, dy = s.ty - ay;
    const double d = std::hypot(dx, dy);
    const double surface = std::max(0.0, d - s.tradius);   // to the body's surface
    if (surface > a.distance) return false;
    const double half = a.angle > 0 ? a.angle * 0.5 : 180.0;
    if (half < 180.0 && d > 1e-9) {
        const double off = std::fabs(wrap(std::atan2(dy, dx) * 180.0 / kPi - axis));
        const double width = std::asin(std::min(1.0, s.tradius / d)) * 180.0 / kPi;   // the body's own width seen from the start of the cone
        if (off - width > half) return false;
    }
    if (a.min_distance > 0 && surface < a.min_distance) return false;
    return true;
}

/// Whether the body overlaps the volume.
inline bool volume_holds(const HitVolume& v, const HitSubject& s) {
    using namespace bite_area_detail;
    const double rad = s.yaw * kPi / 180.0;
    const double c = std::cos(rad), n = std::sin(rad);
    const double vx = s.cx + v.ox * c - v.oy * n, vy = s.cy + v.ox * n + v.oy * c;
    const double dx = s.tx - vx, dy = s.ty - vy;
    if (v.shape == HitVolume::Shape::Sphere) return std::hypot(dx, dy) <= v.radius + s.tradius;
    const double r = (s.yaw + v.yaw) * kPi / 180.0;
    const double lx = dx * std::cos(r) + dy * std::sin(r), ly = -dx * std::sin(r) + dy * std::cos(r);   // into the box's own frame
    return std::hypot(std::max(0.0, std::fabs(lx) - v.hx), std::max(0.0, std::fabs(ly) - v.hy)) <= s.tradius;
}

/// Whether a hit of the record lands on the body.  `is_target`: the body is the one the record's target part finds; `sees`: whether the creature has a line of sight to the body (asked only when
/// something needs it; null: it sees everything).
inline bool hit_lands(const BiteArea& a, const HitSubject& s, bool is_target, const std::function<bool()>& sees) {
    if (a.has_target_part() && is_target) {
        bool ok = true;
        if (a.check_validity) {
            if (a.cone_check && !validity_holds(a, s)) ok = false;
            if (ok && a.los_check && sees && !sees()) ok = false;
        }
        if (ok) return true;
    }
    for (const HitVolume& v : a.volumes) {
        if (volume_holds(v, s) && (!v.los || !sees || sees())) return true;
    }
    return false;
}

}  // namespace aoc::worldd::combat

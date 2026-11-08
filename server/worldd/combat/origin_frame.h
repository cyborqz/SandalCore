// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/origin_frame.h
//
//  The frame a throw or a push works in.  The data names it as an origin transform: a place (an EAbilitySystemOrigin: the owner of the ability, the one who started the hit, the source of it, the target,
//  where it landed, the world's origin), a facing (an EAbilitySystemOrientation: the facing of one of those actors, or the way from one of them towards the place; the "Z" kinds ignore the height of
//  that way) and a location relative to the place in that frame.  A throw's or a push's direction is given in this frame.
//
//  The owner, the instigator and the source of a hit are the same player here; the target is the creature.  Facings count as the world counts a yaw: degrees, 0 along +X, turning towards +Y.
// ============================================================================
#pragma once

#include "combat/fighter.h"
#include "npc/displacement.h"

#include <cmath>
#include <string>

namespace aoc::worldd::combat {

/// An actor in one of the roles an origin transform names: where it stands, which way it faces, how far its body reaches.
struct OriginActor {
    bool   known = false;
    Vec3   pos;
    double yaw_degrees = 0;
    double radius_cm = 0;
};

struct OriginActors {
    OriginActor owner, instigator, source, target;
};

/// A frame: its place, and the yaw and pitch it faces.  `ok` is false when the data names a role or a kind that is not served here.
struct OriginFrame {
    bool   ok = false;
    Vec3   pos;
    double yaw_degrees = 0, pitch_degrees = 0;
};

/// The part of an enum's text after "::".
inline std::string enum_tail(const std::string& text) {
    const size_t at = text.rfind("::");
    return at == std::string::npos ? text : text.substr(at + 2);
}

inline const OriginActor* origin_actor_of(const std::string& role, const OriginActors& a) {
    if (role == "Owner") return &a.owner;
    if (role == "Instigator") return &a.instigator;
    if (role == "Source") return &a.source;
    if (role == "Target") return &a.target;
    return nullptr;
}

/// The frame an origin transform makes for these actors: `location` and `orientation` are the enums' texts, `rel` the relative location in the frame.
inline OriginFrame origin_frame(const std::string& location_text, const std::string& orientation_text, const Vec3& rel, const OriginActors& actors) {
    constexpr double kPi = 3.14159265358979323846;
    OriginFrame out;
    const std::string loc = enum_tail(location_text), ori = enum_tail(orientation_text);
    // where
    if (loc == "World") {
        out.pos = {0, 0, 0};
    } else if (loc == "HitLocation") {
        if (!actors.target.known) return out;
        out.pos = actors.target.pos;   // (a hit lands where its target stands)
    } else {
        const OriginActor* who = origin_actor_of(loc, actors);
        if (!who || !who->known) return out;
        out.pos = who->pos;
    }
    // which way
    if (ori == "World") {
        out.yaw_degrees = 0;
    } else if (const OriginActor* facing = origin_actor_of(ori, actors)) {
        if (!facing->known) return out;
        out.yaw_degrees = facing->yaw_degrees;
    } else if (ori == "TargetZ") {
        if (!actors.target.known) return out;
        out.yaw_degrees = actors.target.yaw_degrees;
    } else {
        // "<Actor>ToOrigin" and "<Actor>ToOriginZ": the way from that actor to the place (the Z kinds level)
        const bool level = ori.size() > 1 && ori.compare(ori.size() - 1, 1, "Z") == 0;
        const std::string head = level ? ori.substr(0, ori.size() - 1) : ori;
        const std::string tail = "ToOrigin";
        if (head.size() <= tail.size() || head.compare(head.size() - tail.size(), tail.size(), tail) != 0) return out;
        const OriginActor* from = origin_actor_of(head.substr(0, head.size() - tail.size()), actors);
        if (!from || !from->known) return out;
        const double dx = out.pos.x - from->pos.x, dy = out.pos.y - from->pos.y, dz = out.pos.z - from->pos.z;
        const double along = std::hypot(dx, dy);
        if (along > 1e-6) out.yaw_degrees = std::atan2(dy, dx) * 180.0 / kPi;
        else out.yaw_degrees = from->yaw_degrees;
        if (!level && (along > 1e-6 || std::fabs(dz) > 1e-6)) out.pitch_degrees = std::atan2(dz, along) * 180.0 / kPi;
    }
    // the relative location is in this frame
    const nav::Vec3 moved = npc::frame_rotate(out.yaw_degrees, out.pitch_degrees, nav::Vec3{rel.x, rel.y, rel.z});
    out.pos = {out.pos.x + moved.x, out.pos.y + moved.y, out.pos.z + moved.z};
    out.ok = true;
    return out;
}

}  // namespace aoc::worldd::combat

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  fishing/fishing.cpp (see fishing.h)
// ============================================================================
#include "fishing/fishing.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::fishing {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

double flight_distance(const Throw& t, double drop_cm, double* flight_seconds) {
    const double rad = t.pitch_deg * kPi / 180.0;
    const double vx = t.speed_cm_s * std::cos(rad), vz = t.speed_cm_s * std::sin(rad);
    const double g = t.gravity_cm_s2;
    const double disc = vz * vz + 2.0 * g * drop_cm;
    if (!(g > 0) || disc < 0) return 0;
    const double time = (vz + std::sqrt(disc)) / g;   // the time at which the lure has come down by `drop_cm` below its launch height
    if (flight_seconds) *flight_seconds = time;
    return time > 0 ? vx * time : 0;
}

std::unique_ptr<Fishing> Fishing::make(const Config& config, const abilities::Catalog& catalog, gather::Gathering& gathering, std::string& error) {
    error.clear();
    std::unique_ptr<Fishing> f(new Fishing());
    f->config_ = config;
    f->ability_ = catalog.ability(kGuidFishingCast);
    if (!f->ability_) {
        error = "the ability tables have no Economy_FishingCast";
        return nullptr;
    }
    if (!f->water_.load(config.water_path, error)) {
        error = "the water file " + config.water_path + " does not read: " + error + " (run scripts/extract_game_data.ps1 -Only tools,waterbodies)";
        return nullptr;
    }
    f->gathering_ = &gathering;
    for (const content::MontageRow& m : f->ability_->montages) {
        if (m.anim_index != 0) continue;
        f->montage_path_ = m.soft_path;
        f->montage_guid_ = m.net_guid ? *m.net_guid : 0;
        break;
    }
    // the projectile's own numbers where the tables hold them (the speed and range); the rest are the constants of Throw
    if (const content::ProjectileRow* p = catalog.projectile(6064632071475822592ll); p && p->speed > 0) {   // Fishing_Projectile
        f->throw_.speed_cm_s = p->speed;
        if (p->range > 0) f->throw_.range_cm = p->range;
    }
    return f;
}

Landing Fishing::plan_cast(double x, double y, double feet_z, double yaw_degrees) const {
    Landing out;
    const double rad = yaw_degrees * kPi / 180.0;
    const double fx = std::cos(rad), fy = std::sin(rad);    // forward
    const double rx = -std::sin(rad), ry = std::cos(rad);   // right (UE's +Y)
    const double ox = x + fx * throw_.forward_cm + rx * throw_.right_cm;
    const double oy = y + fy * throw_.forward_cm + ry * throw_.right_cm;
    const double launch_z = feet_z + throw_.hand_cm;
    // the lure comes down to the surface of the water it lands on, which is not known before it lands: first at the feet's height, then at the surface that was found there
    double surface = feet_z;
    for (int pass = 0; pass < 2; ++pass) {
        const double drop = launch_z - surface;
        double seconds = 0;
        const double reach = flight_distance(throw_, drop, &seconds);
        if (!(reach > 0)) {
            out.why = "the water is above the line's throw";
            return out;
        }
        out.x = ox + fx * reach;
        out.y = oy + fy * reach;
        out.distance_cm = std::hypot(out.x - x, out.y - y);
        out.flight_s = seconds;
        out.water = water_.at(out.x, out.y);
        if (!out.water) {
            out.why = "the line came down on dry land";
            return out;
        }
        if (pass == 1 || std::fabs(out.water->surface_z - surface) < 1.0) break;   // the surface it came down to is the water's
        surface = out.water->surface_z;   // lower (or higher) than the feet: the line flew farther (or less far) to get there
    }
    if (feet_z - out.water->surface_z > throw_.max_drop_cm) {
        out.why = "the water is too far below";
        out.water.reset();
        return out;
    }
    if (out.distance_cm > throw_.range_cm) {
        out.why = "the water is out of the line's range";
        out.water.reset();
        return out;
    }
    out.def = gathering_ ? gathering_->interaction(out.water->fishing) : nullptr;
    if (!out.def) {
        out.why = "the water names fishing data (" + std::to_string(out.water->fishing) + ") that the gathering tables do not have";
        out.water.reset();
        return out;
    }
    out.lands = true;
    return out;
}

const content::GatherDefRow* Fishing::default_def() const {
    const auto data = water_.fishing_data();
    return gathering_ && !data.empty() ? gathering_->interaction(data.front().first) : nullptr;
}

std::string Fishing::describe() const {
    std::string s = std::to_string(water_.bodies()) + " fishable waters (" + std::to_string(water_.lakes()) + " lakes with " + std::to_string(water_.outlines()) + " outlines, " +
                    std::to_string(water_.rivers()) + " rivers), " + std::to_string(water_.boxes()) + " swim volumes; the fishing data:";
    for (const auto& [guid, count] : water_.fishing_data()) {
        const content::GatherDefRow* d = gathering_ ? gathering_->interaction(guid) : nullptr;
        s += " " + (d ? d->name : std::to_string(guid)) + " x" + std::to_string(count) + ";";
    }
    return s;
}

}  // namespace aoc::worldd::fishing

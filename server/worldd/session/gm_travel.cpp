// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/gm_travel.cpp (see gm_travel.h)
// ============================================================================
#include "session/gm_travel.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace aoc { namespace worldd {

namespace {

bool iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string coords(double x, double y) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "(%.0f, %.0f)", x, y);
    return buf;
}

}  // namespace

PlayerHandle GmTravel::find_player(const std::string& name) {
    if (name.empty()) return {};
    for (const PlayerHandle& h : directory_()) {
        if (h && iequal(h.player->name(), name)) return h;
    }
    return {};
}

std::vector<GmPlayerInfo> GmTravel::list_players() {
    std::vector<GmPlayerInfo> out;
    for (const PlayerHandle& h : directory_()) {
        if (!h) continue;
        const PlayerSession& p = *h.player;
        const GoodPosition g = p.last_position();
        GmPlayerInfo info;
        info.name = p.name();
        info.character_id = p.character_id();
        info.position = {g.x, g.y, g.z};
        info.yaw_degrees = g.yaw_degrees;
        info.mounted = p.mounted();
        info.dead = p.dead();
        info.move_speed_multiplier = p.gm_speed();
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<world::Place> GmTravel::list_places() { return world_.places().all(); }
// CoA with C_Y_B

GmTeleportResult GmTravel::teleport(const GmTeleportRequest& request, const std::string& by) {
    GmTeleportResult out;
    if (request.character.empty()) {
        out.status = GmStatus::InvalidArgument;
        out.message = "no character is named";
        return out;
    }
    const PlayerHandle handle = find_player(request.character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "no player named \"" + request.character + "\" is in the world (worldctl players lists them)";
        return out;
    }
    // where to: a place by name, or a point (with the ground's height when no z is given)
    double x = 0, y = 0, z = 0;
    if (!request.place.empty()) {
        const world::Places places = world_.places();
        const world::Place* place = places.find(request.place);
        if (!place) {
            out.status = GmStatus::NotFound;
            out.message = "no place named \"" + request.place + "\" (worldctl places lists them)";
            return out;
        }
        x = place->x;
        y = place->y;
        z = world_.place_height(*place);
        out.place = place->name;
    } else {
        x = request.x;
        y = request.y;
        if (!std::isfinite(request.x) || !std::isfinite(request.y) || (request.has_z && !std::isfinite(request.z))) {
            out.status = GmStatus::InvalidArgument;
            out.message = "the point is not a finite number";
            return out;
        }
        if (request.has_z) {
            z = request.z;
        } else if (const auto ground = world_.ground_at(x, y)) {
            z = *ground + PlayerWorld::kCapsuleHalfHeightCm;
            out.ground_height = true;
        } else {
            out.status = GmStatus::InvalidArgument;
            out.message = "the server has no ground at " + coords(x, y) + ": give z";
            return out;
        }
    }
    const PlayerSession::GmMoveResult moved = handle.player->gm_teleport(*handle.connection, x, y, z, now_(), by);
    out.character = handle.player->name();
    out.from = {moved.from.x, moved.from.y, moved.from.z};
    switch (moved.status) {
        case PlayerSession::GmMoveResult::Status::Moved:
            out.to = {moved.to.x, moved.to.y, moved.to.z};
            out.dismounted = moved.dismounted;
            break;
        case PlayerSession::GmMoveResult::Status::Dead:
            out.status = GmStatus::FailedPrecondition;
            out.message = out.character + " is dead: it comes back first";
            break;
        case PlayerSession::GmMoveResult::Status::OutsideWorld:
            out.status = GmStatus::InvalidArgument;
            out.message = "the point is outside the world";
            break;
        case PlayerSession::GmMoveResult::Status::Busy:
            out.status = GmStatus::FailedPrecondition;
            out.message = out.character + "'s connection is busy: try again in a moment";
            break;
    }
    return out;
}

GmSpeedResult GmTravel::set_speed(const std::string& character, double multiplier, const std::string& by) {
    GmSpeedResult out;
    if (!std::isfinite(multiplier) || multiplier < kMinSpeed || multiplier > kMaxSpeed) {
        out.status = GmStatus::InvalidArgument;
        out.message = "the multiplier must be 0.5 to 10 (1 resets)";
        return out;
    }
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "no player named \"" + character + "\" is in the world (worldctl players lists them)";
        return out;
    }
    const PlayerSession::GmSpeedChange change = handle.player->gm_set_speed(*handle.connection, multiplier, now_(), by);
    out.character = handle.player->name();
    out.previous = change.previous;
    out.multiplier = change.now;
    return out;
}

size_t GmTravel::teleport_characters(const std::string& names, double x, double y, double z, const std::string& by, Clock::time_point now) {
    size_t moved = 0;
    std::string token;
    const auto flush = [&] {
        if (token.empty()) return;
        const PlayerHandle h = find_player(token);
        if (!h) {
            spdlog::warn("[gm] {}: no player named \"{}\" is in the world", by, log_text(token));
        } else if (h.player->gm_teleport(*h.connection, x, y, z, now, by).status == PlayerSession::GmMoveResult::Status::Moved) {
            ++moved;
        }
        token.clear();
    };
    for (const char c : names) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) flush();
        else token.push_back(c);
    }
    flush();
    return moved;
}

}}  // namespace aoc::worldd

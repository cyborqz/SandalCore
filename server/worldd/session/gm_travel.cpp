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
#include <cstdlib>

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

GmMoneyGrant GmTravel::give_money(const std::string& character, int64_t copper, const std::string& by) {
    GmMoneyGrant out;
    if (copper < 1) {
        out.status = GmStatus::InvalidArgument;
        out.message = "the amount must be at least 1 copper (100 copper = 1 silver, 100 silver = 1 gold)";
        return out;
    }
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "no player named \"" + character + "\" is in the world (worldctl players lists them)";
        return out;
    }
    const PlayerSession::GmMoneyResult r = handle.player->gm_give_money(*handle.connection, copper, now_(), by);
    out.character = handle.player->name();
    out.previous = r.previous;
    out.balance = r.balance;
    out.added = r.added;
    if (!r.ok) {
        out.status = GmStatus::FailedPrecondition;
        out.message = r.refusal;
    }
    return out;
}

namespace {

GmStatus gm_status_of(PlayerWorld::StationStatus s) {
    switch (s) {
        case PlayerWorld::StationStatus::Ok: return GmStatus::Ok;
        case PlayerWorld::StationStatus::NotFound: return GmStatus::NotFound;
        case PlayerWorld::StationStatus::Invalid: return GmStatus::InvalidArgument;
        case PlayerWorld::StationStatus::Refused: return GmStatus::FailedPrecondition;
    }
    return GmStatus::FailedPrecondition;
}

GmStationResult station_result_of(const PlayerWorld::StationResult& r) {
    GmStationResult out;
    out.status = gm_status_of(r.status);
    out.message = r.message;
    out.actor = r.actor;
    out.node = r.node;
    out.plot = r.plot;
    out.station = r.station;
    out.building = r.building;
    out.position = {r.x, r.y, r.z};
    return out;
}

}  // namespace

GmStationResult GmTravel::place_station(const std::string& node, const std::string& plot, const std::string& station, const std::string& by) {
    return station_result_of(world_.place_station(log_text(node), log_text(plot), log_text(station), by));
}

GmStationResult GmTravel::remove_station(const std::string& node, const std::string& plot, const std::string& by) {
    return station_result_of(world_.remove_station(log_text(node), log_text(plot), by));
}

GmItemGrant GmTravel::give_item(const std::string& character, const std::string& item, int32_t count, const std::string& by) {
    GmItemGrant out;
    if (item.empty()) {
        out.status = GmStatus::InvalidArgument;
        out.message = "no item is named";
        return out;
    }
    if (count < 1 || count > 100000) {
        out.status = GmStatus::InvalidArgument;
        out.message = "the count must be 1 to 100,000";
        return out;
    }
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "no player named \"" + character + "\" is in the world (worldctl players lists them)";
        return out;
    }
    const content::ContentDb* db = world_.content();
    if (!db) {
        out.status = GmStatus::FailedPrecondition;
        out.message = "worldd has no content.db to look the item up in";
        return out;
    }
    // an item is named by its record name (case does not count) or by its Guid
    int64_t guid = 0;
    const bool digits = std::all_of(item.begin(), item.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (digits && item.size() <= 19) {
        guid = std::strtoll(item.c_str(), nullptr, 10);
    } else {
        std::string error;
        const std::vector<int64_t> found = db->items_named(item, error);
        if (!error.empty()) {
            out.status = GmStatus::FailedPrecondition;
            out.message = error;
            return out;
        }
        if (found.empty()) {
            out.status = GmStatus::NotFound;
            out.message = "no item is named \"" + item + "\" (the name is the record's, such as Consumable_Food_Regen_Rations_T1, or its Guid)";
            return out;
        }
        if (found.size() > 1) {
            out.status = GmStatus::InvalidArgument;
            out.message = "\"" + item + "\" names " + std::to_string(found.size()) + " items: give the Guid of one of them (first: " + std::to_string(found.front()) + ")";
            return out;
        }
        guid = found.front();
    }
    const PlayerSession::GmItemResult r = handle.player->gm_give_item(*handle.connection, guid, count, now_(), by);
    out.character = handle.player->name();
    out.item = r.item_name;
    out.item_guid = guid;
    out.count = r.count;
    out.container = r.container;
    if (!r.ok) {
        out.status = r.item_name.empty() ? GmStatus::NotFound : GmStatus::FailedPrecondition;
        out.message = r.refusal;
    }
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

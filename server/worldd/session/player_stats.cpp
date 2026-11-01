// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks by Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
//
// ============================================================================
//  session/player_stats.cpp
//
//  The game master's stat tools on a character: the stat overrides it carries (the
//  effects the client sees like a gear change, and the engine reads when it plans)
//  and the current values of the stats the pawn holds (Health, Mana).
// ============================================================================
#include "session/player_session.h"

#include <cmath>

namespace aoc { namespace worldd {

bool PlayerSession::set_gm_effects(const std::vector<stats::ActiveEffect>& effects, std::string& err) {
    if (!stats_ || !stats_->started()) { err = "the character has no stats"; return false; }
    const std::vector<stats::ActiveEffect> before = stats_->gm_effects();
    stats_->set_gm_effects(effects);
    const size_t failed = stat_updates_failed_;
    apply_gear_stats("the game master's stat overrides");
    if (stat_updates_failed_ != failed) {
        stats_->set_gm_effects(before);   // the client refused the change: the overrides stay as they were, nothing is left behind
        err = "the engine refused the stat overrides";
        return false;
    }
    abilities_dirty_ = true;   // the pawn's stats changed: the deltas go out with the next ability update (a gear change queues its own)
    return true;
}

void PlayerSession::clear_gm_effects() {
    if (!stats_) return;
    const std::vector<stats::ActiveEffect> before = stats_->gm_effects();
    stats_->set_gm_effects({});
    const size_t failed = stat_updates_failed_;
    apply_gear_stats("the game master's stat overrides were cleared");
    if (stat_updates_failed_ != failed) {   // the client refused the change: the overrides stay
        stats_->set_gm_effects(before);
        return;
    }
    abilities_dirty_ = true;   // the pawn's stats changed: the deltas go out with the next ability update
}

const std::vector<stats::ActiveEffect>& PlayerSession::gm_effects() const {
    static const std::vector<stats::ActiveEffect> kEmpty;
    return stats_ ? stats_->gm_effects() : kEmpty;
}

std::vector<stats::StatValue> PlayerSession::player_stats() const {
    if (!stats_) return {};
    return stats_->values();
}

bool PlayerSession::set_player_current_stat(int64_t guid, double value, std::string& err) {
    if (!stats_ || !stats_->started()) { err = "the character has no stats"; return false; }
    const stats::StatValue* stat = stats_->find(guid);
    if (!stat) { err = "the character has no such stat"; return false; }
    if (!stat->held()) { err = "that stat has no current value to set"; return false; }
    if (!std::isfinite(value)) { err = "the value is not a number"; return false; }
    if (!pb_.has_stat_item(guid)) { err = "the client has no item for that stat"; return false; }
    // the pawn's arrays take the value, the path the game's own current values take (a hit, a fall, a respawn): the change goes out with the next update. The gear
    // stats' path cannot carry it: it computes its delta against the session's stats, and the current value is already in them.
    if (!set_current_value(guid, static_cast<float>(value))) { err = "the pawn does not take the value"; return false; }
    return true;
}

}}  // namespace aoc::worldd

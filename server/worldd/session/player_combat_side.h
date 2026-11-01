// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_combat_side.h
//
//  The player's side of the area's fights: what the world's fights see of a joined player (a Fighter and a PlayerPort).  Defined here because both
//  the combat file and the ability file of the player session use it.
// ============================================================================
#pragma once

#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "crafting/job.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "world/city_nodes.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "items/acquired.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "world/zoi_surface.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

namespace aoc { namespace worldd {

using protocol::emit::FIntrepidNetworkGUID;

/// The world's view of this session: a Fighter (where the player stands, whether it lives, the stats the formulas run on) and a PlayerPort (what NPCs do to it,
/// queued here and applied by the session's own tick, which has the connection).
class PlayerSession::CombatSide final : public combat::PlayerPort, public combat::Fighter {
public:
    explicit CombatSide(PlayerSession& s) : s_(s), aligns_(combat::CombatData::player_alignments()) {}

    // PlayerPort
    uint64_t actor_id() const override { return s_.pb_.setup().guids.pawn.ObjectId; }
    combat::Fighter& fighter() override { return *this; }
    void receive_hit(const combat::IncomingHit& hit, double) override {
        if (inbox.size() < 256) inbox.push_back(hit);
    }
    void set_in_combat(bool in_combat, double) override { wants_in_combat = in_combat; }
    std::array<uint32_t, 4> character_guid() const override { return s_.pb_.setup().character_guid; }
    FIntrepidNetworkGUID pawn_guid() const override { return s_.pb_.setup().guids.pawn; }
    FIntrepidNetworkGUID cylinder_guid() const override { return s_.pb_.setup().guids.collision_cylinder; }
    void receive_kill(const combat::KillReward& kill, double) override {
        if (kills.size() < 64) kills.push_back(kill);
    }
    void confirm_effect_hit(const combat::EffectHitNote& note, double) override {
        if (effect_notes.size() < 256) effect_notes.push_back(note);
    }

    // Fighter
    uint64_t id() const override { return actor_id(); }
    bool is_player() const override { return true; }
    int level() const override { return s_.stats_ ? s_.stats_->level() : 1; }
    combat::Vec3 position() const override {
        const GoodPosition& g = s_.checker_.good();
        return {g.x, g.y, g.z};
    }
    double radius_cm() const override { return 34.0; }
    double facing_degrees() const override { return s_.checker_.good().yaw_degrees; }   // the control rotation's yaw, as the client's last good move gave it
    bool alive() const override {
        const stats::StatValue* h = s_.stats_ ? s_.stats_->find(content::kGuidHealth) : nullptr;
        return h && h->value > 0;
    }
    const std::vector<int64_t>& alignments() const override { return aligns_; }
    stats::CharacterStats& stats() override { return *s_.stats_; }
    std::vector<stats::WornItem> worn() const override { return bootstrap::worn_items_of(s_.inventory_); }

    std::vector<combat::IncomingHit> inbox;
    std::vector<combat::KillReward>  kills;
    std::vector<combat::EffectHitNote> effect_notes;   // the hits of the effects this player put on creatures, to confirm to his client
    bool                             wants_in_combat = false;

private:
    PlayerSession&       s_;
    std::vector<int64_t> aligns_;
};

}}  // namespace aoc::worldd

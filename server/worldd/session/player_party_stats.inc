// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  session/player_party_stats.inc: the party's live stats (social, seeing other players: live Health and Mana; included at the end of player_party.inc)
//
//  Two things follow the members' values, each on a cadence of its own:
//    the social component   the SocialComponent's PlayerStatsReplication item of each member (his character GUID as the key and the nine PlayerStats values), sent
//                           through friends_update(): at most one update a second per session, carrying every member that changed since the last one, so a party of
//                           eight sends one update a second per client and not one a member.  A member who leaves loses his item, and a session that leaves its party
//                           empties the array;
//    the party frame        ClientUpdateGroup's PlayerStats and places: at once when a member's level, alive state or maxima change, and with the roster's refresh
//                           (about 4.5 s, the recorded frame's mean) for the places and the shares.
//
//  Modelled: no recording decodes either payload, so the shapes are the game's own schema's (the nine values of the party frame's PlayerStats), the alive status is 0
//  alive and 1 dead, and the percentages are the current value over its maximum.
// ============================================================================

namespace {

/// The most often a session sends its party's live stats: one update a second, whatever the members' changes are.
constexpr auto kPartyStatsInterval = std::chrono::milliseconds(1000);
/// The most often a session sends the party frame: a level, an alive state or a maximum at once, the places and the shares with the roster's refresh.
constexpr auto kPartyFrameMinInterval = std::chrono::milliseconds(1000);
constexpr auto kPartyFrameRefresh = std::chrono::milliseconds(4500);

/// The group's members as the frame and the social component carry them, in the group's order.
std::vector<bootstrap::GroupMember> party_member_data(const PlayerSession::PartyGroup& group) {
    std::vector<bootstrap::GroupMember> out;
    out.reserve(group.members.size());
    for (PlayerSession* member : group.members) {
        if (member) out.push_back(party_member_of(*member));
    }
    return out;
}

/// True when `now` and `before` differ in a value a roster frame must not wait for: a member's level, alive state or maxima.
bool party_durable_change(const std::vector<bootstrap::GroupMember>& before, const std::vector<bootstrap::GroupMember>& now) {
    if (before.size() != now.size()) return true;
    for (size_t i = 0; i < now.size(); ++i) {
        if (before[i].level != now[i].level || before[i].alive_status != now[i].alive_status || before[i].max_health != now[i].max_health || before[i].max_mana != now[i].max_mana) return true;
    }
    return false;
}

}  // namespace

void PlayerSession::note_party_frame_sent(std::vector<bootstrap::GroupMember> members) {
    party_frame_sent_ = std::move(members);
    party_frame_refresh_due_ = last_now_ + kPartyFrameRefresh;   // (a frame just went out: the places and the shares wait a roster period for the next, not a tick)
}

void PlayerSession::sync_party_stats(Connection& conn, Clock::time_point now) {
    std::vector<bootstrap::GroupMember> members;
    if (party_group_) members = party_member_data(*party_group_);
    if (members.empty() && party_stats_sent_.empty()) return;   // (in no party, and the array holds nothing of one)
    if (members == party_stats_sent_) return;                   // (nothing changed)
    if (now < party_stats_due_) return;                         // (one update a second at most)
    if (!pb_.set_party_stats(members)) {
        if (++party_sync_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: the social component does not take the party's stats ({}): the party window keeps the values it has", name(),
                         pb_.error().empty() ? "no reason" : pb_.error());
        }
        return;   // (tried again at the next tick)
    }
    queue("SocialComponent (party stats)", [](bootstrap::PlayerBootstrap& pb) { return pb.friends_update(); }, conn);
    party_stats_sent_ = std::move(members);
    party_stats_due_ = now + kPartyStatsInterval;
    ++party_stats_sends_;
}

void PlayerSession::sync_party_frame(Connection& conn, Clock::time_point now) {
    if (!party_group_) {
        party_frame_sent_.clear();
        return;
    }
    std::vector<bootstrap::GroupMember> members = party_member_data(*party_group_);
    if (members == party_frame_sent_) return;   // (nothing changed)
    if (now < party_frame_due_) return;         // (two frames at least a second apart)
    if (!party_durable_change(party_frame_sent_, members) && now < party_frame_refresh_due_) return;   // (the places and the shares wait for the refresh)
    const bootstrap::GroupClientData data = party_frame_data(*party_group_);
    queue("ClientUpdateGroup (the party's stats)", [data](bootstrap::PlayerBootstrap& pb) { return pb.group_update(data); }, conn);
    party_frame_sent_ = std::move(members);
    party_frame_due_ = now + kPartyFrameMinInterval;
    party_frame_refresh_due_ = now + kPartyFrameRefresh;
    ++party_frame_sends_;
}

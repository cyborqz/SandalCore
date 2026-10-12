// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/gm_travel.h  (the GM travel tools: teleport and run speed)
//
//  "I need a faster way to get around": a GM moves a player to a city node or a point, and sets a character's run speed.  Two doors lead to the same code:
//
//    the admin API   proto/internal/aoc/worldadmin/v1 (svc/world_admin.h serves it, services/cmd/worldctl is its command line, the admin app to come reuses it): a call
//                    arrives on a gRPC thread and is applied on worldd's network thread (svc/world_admin.h's AdminQueue), where GmTravel below runs: no session state is touched
//                    off-thread.
//    the client's    ServerTeleportXYZ (rpc 967: X, Y, Z whole cm), ServerTeleport (964: a DestinationLookupKey, an OptionalRadius) and ServerTeleportPlayer (966:
//    own GM RPCs     CharacterNames, X, Y, Z) on the PlayerController, answered by PlayerSession with the same teleport, ONLY with the GM switch on (--gm-commands /
//                    AOC_GM_COMMANDS=1, PlayerWorld::gm_commands; off by default).  Whether the shipping client's console can send them at all is not known: each one received
//                    is logged, switch or no switch.
//
//  Teleport.  The path E1's respawn uses to move a player: the move checker's teleport() (the last good position and the credits start over, the TimeStamp state is kept) and a
//  position correction to the client (ClientAdjustPosition), so the client is moved and its next moves are measured from the new place, not corrected.  A mounted player is
//  dismounted first (the ride is over); the mount stays where it was and desummons by distance as it does today (PlayerSession::run_mounts).  Every teleport is logged with its from
//  and to.  A dead player is not moved.  The place is the ground when no z is given: the landscape's height under the point plus the capsule's half height (98 cm), when the server has
//  ground there, else the call is refused and asks for z; a named place stands 98 cm above the ground, or 150 cm above its recorded z where the server has no ground.  Inside a city
//  node's bowl (Winstead, Joeva, New Aela, Halcyon, Miraleth: the baked terrain is a pit about 50 m under the town) the ground is the town's floor plus 2 m of slack (PlayerWorld::ground_at,
//  world/town_floor.h); a z that is given is believed.
//
//  Run speed.  A multiplier of MoveSpeedMult, 0.5 to 10, lasts until the session ends (relog) or it is set to 1.  On foot it is the pawn's replicated MoveSpeedMult stat (the one
//  the Sprint's effect raises: the client moves at 600 cm/s times it), on a mount the mount's own MoveSpeedMult (the stat the road's and the dash's effects raise), so the client
//  really moves faster; the move checker's limits scale with it (a multiplier below 1 never lowers them: a Dodge's dash is not slowed by it), with a grace of a second after the
//  speed goes down, while the client learns of it.
// ============================================================================
#pragma once

#include "net/connection.h"
#include "session/player_session.h"
#include "session/player_world.h"
#include "world/places.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

/// A client's text for the log: a control character (a newline would start a log line of the player's making) becomes '?'.  Every free string a player sends and worldd logs goes
/// through it (Nodes 1 review A, M3).
inline std::string log_text(std::string s) {
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
    }
    return s;
}

/// What a session asks of the rest of the world for a client GM command that names other characters.  Runs on the network thread.
class GmPort {
public:
    virtual ~GmPort() = default;
    /// ServerTeleportPlayer: moves the characters named (separated by commas, semicolons or spaces) to the point; returns how many were moved.
    virtual size_t teleport_characters(const std::string& names, double x, double y, double z, const std::string& by, Clock::time_point now) = 0;
};

/// How a call ended, in gRPC's terms (svc/world_admin.cpp maps them).
enum class GmStatus : uint8_t { Ok, NotFound, InvalidArgument, FailedPrecondition };

struct GmPosition {
    double x = 0, y = 0, z = 0;
};

struct GmPlayerInfo {
    std::string name, character_id;
    GmPosition  position;
    double      yaw_degrees = 0;
    bool        mounted = false, dead = false;
    double      move_speed_multiplier = 1.0;
};

struct GmTeleportRequest {
    std::string character;
    std::string place;           // a place's name; empty: the point below
    double      x = 0, y = 0, z = 0;
    bool        has_z = false;   // false: z is the ground height under (x, y), when the server has ground there
};
// (C | O | A ~ C.Y.B)

struct GmTeleportResult {
    GmStatus    status = GmStatus::Ok;
    std::string message;         // why it was refused
    std::string character;       // the name worldd has the player under
    GmPosition  from, to;
    std::string place;           // the place's name when a place was asked for
    bool        dismounted = false;
    bool        ground_height = false;
};

struct GmSpeedResult {
    GmStatus    status = GmStatus::Ok;
    std::string message;
    std::string character;
    double      previous = 1.0, multiplier = 1.0;
};

/// The GM travel tools on the network thread.  `directory` lists the players of the world (Endpoint::players), `now` is the Endpoint's clock.
class GmTravel : public GmPort {
public:
    using Directory = std::function<std::vector<PlayerHandle>()>;
    using Now = std::function<Clock::time_point()>;

    /// The run speed bounds (the proto says them too).
    static constexpr double kMinSpeed = 0.5, kMaxSpeed = 10.0;

    GmTravel(PlayerWorld& world, Directory directory, Now now) : world_(world), directory_(std::move(directory)), now_(std::move(now)) {}

    std::vector<GmPlayerInfo> list_players();
    std::vector<world::Place> list_places();
    /// `by` names who asked, for the log ("worldctl", "ServerTeleportXYZ").
    GmTeleportResult teleport(const GmTeleportRequest& request, const std::string& by);
    GmSpeedResult    set_speed(const std::string& character, double multiplier, const std::string& by);

    size_t teleport_characters(const std::string& names, double x, double y, double z, const std::string& by, Clock::time_point now) override;

    /// The player named `name` (case does not count), or an empty handle.
    PlayerHandle find_player(const std::string& name);

private:
    PlayerWorld& world_;
    Directory    directory_;
    Now          now_;
};

}}  // namespace aoc::worldd

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

/// The vendors milestone, step 0: the test commands that give money and items.
struct GmMoneyGrant {
    GmStatus    status = GmStatus::Ok;
    std::string message;
    std::string character;
    int64_t     previous = 0, balance = 0, added = 0;
};

struct GmItemGrant {
    GmStatus    status = GmStatus::Ok;
    std::string message;
    std::string character;
    std::string item;        // the record's name
    int64_t     item_guid = 0;
    int32_t     count = 0;
    std::string container;   // where the first stack went
    int32_t     quality = 0;   // the quality the stacks were given at
    std::string rarity;        // ... and the rarity the player sees them in
};

/// Crafting, step 0: the test commands that put an artisan building's station on a free plot, and take it off.
struct GmStationResult {
    GmStatus    status = GmStatus::Ok;
    std::string message;
    uint64_t    actor = 0;
    std::string node, plot, station, building;
    GmPosition  position;
};

/// Crafting, step 2: the test command that finishes a player's processing jobs at once.
struct GmFinishResult {
    GmStatus    status = GmStatus::Ok;
    std::string message;
    std::string character;
    uint32_t    finished = 0;   // the jobs finished (running and waiting ones)
};

/// Crafting, step 3: the test commands that teach a player a recipe the game makes a player learn, take it away again, and list what was taught.
struct GmRecipeResult {
    GmStatus    status = GmStatus::Ok;
    std::string message;
    std::string character;
    std::string recipe;        // the recipe record's name
    int64_t     recipe_guid = 0;
    int32_t     taught = 0;    // how many recipes the character has been taught now
};
struct GmTaughtList {
    GmStatus    status = GmStatus::Ok;
    std::string message;
    std::string character;
    std::vector<std::pair<std::string, int64_t>> recipes;   // name and Guid, in the order they were taught
};

/// Missing systems, step 0: the test command that puts a player's finished tutorials back.
struct GmTutorialResult {
    GmStatus    status = GmStatus::Ok;
    std::string message;
    std::string character;
    uint32_t    cleared = 0;   // the finished tutorials put back
};

/// The NPC actions: a game master's tools on one creature, named by its actor id.  One shape serves all six; the fields an action does not fill stay empty.
struct GmNpcResult {
    GmStatus    status = GmStatus::Ok;
    std::string message;                 // why it was refused, or one plain sentence of what was done
    uint64_t    id = 0;                  // the creature's actor id after the call (a new one when a living creature was replaced)
    std::string name;                    // its nameplate
    GmPosition  position;                // where it stands now (where it stood, for one that is gone)
    GmPosition  from;                    // a move: where it stood
    uint32_t    loot_stacks = 0;         // a kill: the stacks on its corpse
    double      respawn_in_seconds = 0;  // a kill or a despawn: until its spawn point makes it again (0: it does not)
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
    /// Adds copper to a player's purse (the cap is 10,000,000 copper) and shows it; the amount is 1 or more.
    GmMoneyGrant     give_money(const std::string& character, int64_t copper, const std::string& by);
    /// Gives a player `count` of an item named by its record name or its Guid (digits), routed by the bag rule.
    /// `rarity` (a name) or `quality` (1 to 200) says which quality the item is given at; both empty: its default quality (PlayerSession::gm_give_item).
    GmItemGrant      give_item(const std::string& character, const std::string& item, int32_t count, const std::string& by, const std::string& rarity = std::string(), int32_t quality = 0);
    /// Puts the station `station` of an artisan building on the free plot `plot` of the node `node` (a Village or a Town), and takes it off again.
    GmStationResult  place_station(const std::string& node, const std::string& plot, const std::string& station, const std::string& by);
    GmStationResult  remove_station(const std::string& node, const std::string& plot, const std::string& by);
    /// Finishes the processing jobs of a player that are not over (running and waiting ones), all of them or only those at the station named by `station` (its Blueprint's or its definition's
    /// name; empty for all): each is over now and ready to collect.
    GmFinishResult   finish_jobs(const std::string& character, const std::string& station, const std::string& by);
    /// Teaches a player a recipe (named by its record name or its Guid in digits) that the game makes a player learn, takes a taught one away, and lists the taught ones.
    GmRecipeResult   teach_recipe(const std::string& character, const std::string& recipe, const std::string& by);
    GmRecipeResult   forget_recipe(const std::string& character, const std::string& recipe, const std::string& by);
    GmTaughtList     list_taught(const std::string& character);
    /// Puts a player's finished tutorials back: the list is emptied, the client is told (its hints can start again) and the empty list is saved.
    GmTutorialResult reset_tutorials(const std::string& character, const std::string& by);

    // ---- the NPC actions (a creature is named by its actor id, as the world stream and Inspect give it)
    /// Kills it as a blow would: the same death and corpse, the loot a kill gives (rolled as for a player of the creature's own level, open to every player in the world, no experience), and its
    /// spawn point makes it again on its usual timer.
    GmNpcResult kill_npc(uint64_t id, const std::string& by);
    /// A dead creature (its corpse still stands, or only its timer is left) is made again at its spawn point now; a living one is replaced by a fresh one there.
    GmNpcResult respawn_npc(uint64_t id, const std::string& by);
    /// A living creature goes back to its spawn point at a run, healed, its threat cleared and its leash centred on the spawn point again.
    GmNpcResult reset_npc(uint64_t id, const std::string& by);
    GmNpcResult heal_npc(uint64_t id, const std::string& by);
    /// Takes it out of the world, corpse and all; its spawn point makes it again on its usual timer.
    GmNpcResult despawn_npc(uint64_t id, const std::string& by);
    /// Puts a living creature at (x, y) at once, on the ground there (or at `z` when `has_z`); its leash and its walk home are centred on the point from then on; its spawn point stays.
    GmNpcResult move_npc(uint64_t id, double x, double y, double z, bool has_z, const std::string& by);

    size_t teleport_characters(const std::string& names, double x, double y, double z, const std::string& by, Clock::time_point now) override;

    /// The player named `name` (case does not count), or an empty handle.
    PlayerHandle find_player(const std::string& name);

private:
    PlayerWorld& world_;
    Directory    directory_;
    Now          now_;
};

}}  // namespace aoc::worldd

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  worldd: the world server (plan A4: the connection layer, plan A5: the native bootstrap)
//
//  Registers with mesh, accepts game clients on UDP, redeems their join
//  tickets with the gateway and loads their characters, and takes them
//  through the control channel's login.  After NMT_Join it spawns the player
//  natively (session/player_session.h): the character's actors go out on actor
//  channels and the client's requests are answered.  That needs content.db
//  (--content-db, plan C1's contentc output); without it worldd still starts
//  but refuses every login.  Plan A6: each move is checked and a player's last good
//  position is saved through the character service on a worker thread (session/
//  position_saver.h), also when worldd stops.
//
//      worldd --bind 127.0.0.1:7777 --service-token-file dist/services/service.token
// ============================================================================
#include "login/login_service.h"
#include "login/login_worker.h"
#include "net/cookie.h"
#include "net/endpoint.h"
#include "net/udp_socket.h"
#include "protocol/server_guid_allocator.h"
#include "session/player_world.h"
#include "session/gm_travel.h"
#include "session/position_saver.h"
#include "spawns/node_population.h"
#include "svc/city_node_feed.h"
#include "svc/grpc_services.h"
#include "svc/service_token.h"
#include "svc/world_admin.h"
#include "svc/city_node_control.h"
#include "world/city_nodes.h"
#include "world/node_layout_data.h"
#include "world/node_scenery.h"
#include "world/node_wire.h"
#include <CLI/CLI.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <optional>
#include <thread>
// CoA - C.Y.B

using namespace aoc::worldd;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// Sleeps up to `d`, waking early when stopping.
void sleep_unless_stopped(std::chrono::milliseconds d) {
    const auto until = Clock::now() + d;
    while (!g_stop && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"worldd: the AoC world server"};
    std::string bind = "127.0.0.1:7777";
    std::string public_address;
    std::string mesh = "127.0.0.1:50053";
    std::string gateway = "127.0.0.1:50054";
    std::string character = "127.0.0.1:50052";
    std::string citynode = "127.0.0.1:50055";
    std::string citynode_seed = "deploy/citynode_seed.json";
    bool no_citynode = false;
    std::string token_file = "dist/services/service.token";
    std::string log_file;
    std::string content_db_path = "dist/content/content.db";
    // tier2: tier 1 starts the world, and the client also needs CombatSettings for the PvP opt-in protection effect
    // (in-game checks 2026-10-01: tier 1 played but logged "Failed to find effect record to display PVP opt-in
    // protection"; tier 2 did not).  tier3 (Nodes 1, stage 2): tier 2 plus the node maps of retail's GameState and the node fields of OwnerInfo: without them the client logs
    // a "Record not found: CityNodeRecord" at every zone banner and finds no node for its audio.
    std::string gamestate_mode = "tier3";
    EndpointConfig endpoint_config;
    MovementConfig movement;
    app.add_option("--bind", bind, "UDP address to listen on");
    app.add_option("--public-address", public_address,
                   "address game clients dial, registered with mesh (default: --bind)");
    app.add_option("--mesh", mesh, "mesh gRPC address");
    app.add_option("--gateway", gateway, "gateway internal gRPC address (RedeemTicket)");
    app.add_option("--character", character, "character service gRPC address");
    // Nodes 1, stage 1: the city nodes' live state (services/cmd/citynode); worldd watches it and runs with the seed's levels when it is not there
    app.add_option("--citynode", citynode, "citynode service gRPC address (WatchNodes)")->envname("AOC_CITYNODE_GRPC_ADDR")->capture_default_str();
    app.add_option("--citynode-seed", citynode_seed,
                   "the node seed (deploy/citynode_seed.json): the levels worldd uses until the citynode service answers, and while it cannot be reached")
        ->envname("AOC_CITYNODE_SEED")
        ->capture_default_str();
    app.add_flag("--no-citynode", no_citynode, "do not call the citynode service: the nodes stay at the seed's levels")->envname("AOC_NO_CITYNODE");
    // the GM travel tools (session/gm_travel.h): the admin API worldctl and the admin app to come call, and the switch for the client's own GM teleport commands
    std::string world_admin = "127.0.0.1:50056";
    bool no_world_admin = false, gm_commands = false;
    app.add_option("--world-admin", world_admin, "the address worldd's admin API (WorldAdminService: list players and places, teleport, run speed; services/cmd/worldctl) listens on; calls need the service token")
        ->envname("AOC_WORLD_ADMIN_ADDR")
        ->capture_default_str();
    app.add_flag("--no-world-admin", no_world_admin, "do not serve the admin API")->envname("AOC_NO_WORLD_ADMIN");
    app.add_flag("--gm-commands", gm_commands,
                 "answer the game client's own GM teleport commands (ServerTeleportXYZ, ServerTeleport, ServerTeleportPlayer on the PlayerController); off by default. Each one the client sends is logged either way")
        ->envname("AOC_GM_COMMANDS");
    app.add_option("--service-token-file", token_file, "the internal services' shared token");
    app.add_option("--log-file", log_file, "also write the log to this file");
    app.add_option("--content-db", content_db_path,
                   "content.db, the game numbers contentc builds (cd services && go run ./cmd/contentc); without it every login is refused")
        ->envname("AOC_CONTENT_DB")
        ->capture_default_str();
    app.add_option("--gamestate-mode", gamestate_mode,
                   "how much of the GameState a joining player gets: tier1, tier2, tier3 (tier 2 and the city nodes: the default) or captured (the original's whole "
                   "open: an escape hatch for in-game debugging only)")
        ->envname("AOC_GAMESTATE_MODE")
        ->transform(CLI::IsMember({"tier1", "tier2", "tier3", "captured"}, CLI::ignore_case))
        ->capture_default_str();
    app.add_option("--max-move-speed", movement.limits.max_speed,
                   "the fastest a player may move along the ground, cm/s: a move that is further from the last good position than "
                   "this allows is refused and the client put back. The default is 1.5 x the fastest the captures show; lower it "
                   "(say 500) to see the corrections in game")
        ->envname("AOC_MAX_MOVE_SPEED")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    // plan E1: the living area. The centre is the login spot (spawns/area.h); the --area-radius is the one place the area's shape is set.
    // G1 stage 1: the default is the whole Riverlands (49 km^2); a radius > 0 is E1's circle around the login spot.
    spawns::AreaConfig area;
    double area_radius_m = -1;
    app.add_option("--area-radius", area_radius_m,
                   "the area filled with NPCs, resource nodes and the GM vendors: -1 (the default) the whole Riverlands (the 12 zones of influence of that biome, 49 km^2); "
                   "a radius in metres > 0 a circle of that radius around the login spot (E1's was 1000); 0: no other actors")
        ->envname("AOC_AREA_RADIUS_M")
        ->check(CLI::Range(-1.0, 20000.0))
        ->capture_default_str();
    // G1 stage 1: the kill switch for the zones of influence in the spawn filter
    bool no_zone_filter = false;
    app.add_flag("--no-zone-filter", no_zone_filter,
                 "the Riverlands' points have no zone of influence: a spawn filter that names one (11 of them: Aela, Winstead, NewAela) never passes, as in E1's circle")
        ->envname("AOC_NO_ZONE_FILTER");
    // G1 stage 1: the GM garden (spawns/gm_garden.h), off by default
    bool gm_garden = false;
    app.add_flag("--gm-garden", gm_garden,
                 "a GM garden near the login spot: one resource node for each gathering profession (Mining, Lumberjacking, Herbalism) and certification tier "
                 "(Novice to GrandMaster), 15 in all, to test every tier; visible only (gathering is G1 stage 2). Off by default")
        ->envname("AOC_GM_GARDEN");
    // Nodes 1, stage 3a: the towns' scenery (world/node_scenery.h)
    bool no_node_scenery = false;
    std::string node_origin = "captured";
    double scenery_radius_m = 1450.0;
    app.add_flag("--no-node-scenery", no_node_scenery, "no town replicators: the nodes' buildings and props are not sent (the towns are empty holes, as before stage 3a)")->envname("AOC_NO_NODE_SCENERY");
    app.add_option("--node-origin", node_origin,
                   "where a town's replicator stands: captured (the default: the open locations of the 13 nodes the retail captures show, the node record's location for the rest) or "
                   "record (every node at its record's location; the layout may sit 6 to 40 m off)")
        ->envname("AOC_NODE_ORIGIN")
        ->transform(CLI::IsMember({"captured", "record"}, CLI::ignore_case))
        ->capture_default_str();
    // Nodes 1, stage 3b: the towns' people (spawns/node_populations.h) and the roads by node level
    bool no_node_people = false;
    std::string roads_levels_path = "dist/content/roads_levels.tsv";
    app.add_flag("--no-node-people", no_node_people, "no town people: the layouts' and buildings' populations (vendors, clerks, guards, villagers) are not spawned, and a node's level does not change them")
        ->envname("AOC_NO_NODE_PEOPLE");
    app.add_option("--roads-levels", roads_levels_path,
                   "the roads of every node level with their owner zones (contentc roads --levels): a road is worth what its zone's node level makes it; used instead of --roads when the file exists")
        ->envname("AOC_ROADS_LEVELS")
        ->capture_default_str();
    app.add_option("--scenery-radius", scenery_radius_m, "a town's replicator is sent to a player within this many metres of it (retail opened it at 1,190 to 1,450 m)")
        ->envname("AOC_SCENERY_RADIUS_M")
        ->check(CLI::Range(100.0, 10000.0))
        ->capture_default_str();
    // plan E1, stage 2: the ground (tools/mapc's area_nav.bin) and how the NPCs move on it
    std::string area_nav_path = "dist/content/area_nav.bin";
    std::string npc_motion_path = "dist/content/npc_motion.tsv";
    bool no_npc_movement = false;
    app.add_option("--area-nav", area_nav_path, "the ground of the area: the navmesh and the terrain tools/mapc builds (a missing file: the NPCs stand where the data puts them)")
        ->envname("AOC_AREA_NAV")
        ->capture_default_str();
    app.add_option("--npc-motion", npc_motion_path, "the NPC archetypes' capsules and speeds tools/mapc reads from the game (a missing file: the Blueprint defaults)")
        ->envname("AOC_NPC_MOTION")
        ->capture_default_str();
    // the mounts milestone: the speeds of the mount Blueprints (tools/mapc mount-speeds), what a desummoned mount runs at
    std::string mount_speeds_path = "dist/content/mount_speeds.tsv";
    app.add_option("--mount-speeds", mount_speeds_path, "the mount Blueprints' speeds tools/mapc reads from the game (a missing file: 600 cm/s times the item's ground percentage)")
        ->envname("AOC_MOUNT_SPEEDS")
        ->capture_default_str();
    // the mounts milestone, stage 3 fix 2: the rider AnimBPs of the mount Blueprints (tools/mapc mount-speeds writes them with the speeds): the AnimBP a rider's mesh is put on
    std::string mount_riders_path = "dist/content/mount_riders.tsv";
    app.add_option("--mount-riders", mount_riders_path, "the mount Blueprints' rider AnimBPs tools/mapc reads from the game (a missing file: each mount names the AnimBP of its kind's retail donor)")
        ->envname("AOC_MOUNT_RIDERS")
        ->capture_default_str();
    // the mounts milestone, stage 4: the roads (tools/mapc `mapc roads`, then `contentc roads --node-level ...` write dist/content/roads.tsv): a mount on a road rides 10 to 30 % faster
    std::string roads_path = "dist/content/roads.tsv";
    app.add_option("--roads", roads_path, "the area's roads, dist/content/roads.tsv (mapc roads, then contentc roads --node-level <the --spawn-node-level>; a missing file: no roads, a mount rides at its own speed)")
        ->envname("AOC_ROADS")
        ->capture_default_str();
    app.add_flag("--no-npc-movement", no_npc_movement, "NPCs stand still where the data puts them (stage 1's behaviour)");
    // plan E1, stage 2 fix: the NPCs no retail capture opened are made from their Blueprints (spawns/generic_npc.h)
    bool no_generic_npcs = false;
    std::string npc_exclude;
    app.add_flag("--no-generic-npcs", no_generic_npcs, "only the NPCs whose Blueprint a retail capture opened spawn (stage 1's NPCs); the rest are not made")
        ->envname("AOC_NO_GENERIC_NPCS");
    app.add_option("--npc-exclude", npc_exclude,
                   "the kill switch for one NPC type: Blueprint names (Goblin_Gravehide, Hawk_C ...) or archetype paths, comma separated, that are not spawned "
                   "(the log counts each type that is)")
        ->envname("AOC_NPC_EXCLUDE");
    // plan E1, stage 3: the spawn filter (spawns/spawn_filter.h): only what retail spawns for a character here now
    bool no_spawn_filter = false, spawn_humanoids = false;
    double spawn_hour = -1;
    std::string spawn_node_level = "Wilderness";
    app.add_flag("--no-spawn-filter", no_spawn_filter, "spawn every point of the design data whose Blueprint opens, as stage 2 did (quest and event spawns, other maps' spawns, humanoids with no look ...)")
        ->envname("AOC_NO_SPAWN_FILTER");
    app.add_flag("--spawn-humanoids", spawn_humanoids, "let the humanoid Blueprints through whose body needs an NPCAppearanceComponent the open does not carry (they show invisible)")
        ->envname("AOC_SPAWN_HUMANOIDS");
    app.add_option("--spawn-hour", spawn_hour, "the game hour (0-24) the spawns' time-of-day conditions are judged at (default: the machine's local hour)")
        ->envname("AOC_SPAWN_HOUR")
        ->check(CLI::Range(-1.0, 24.0));
    app.add_option("--spawn-node-level", spawn_node_level, "the node level the spawns' conditions are judged at: Wilderness, Crossroads, Encampment, Village, Town, City, Metropolis")
        ->envname("AOC_SPAWN_NODE_LEVEL")
        ->capture_default_str();
    // plan E1, stage 3: the fights (combat/combat_world.h)
    bool no_combat = false;
    double npc_aggro_cm = 800.0, npc_leash_cm = 3000.0;
    app.add_flag("--no-combat", no_combat, "the kill switch for the fights: the NPCs do not aggro, attack or take hits (stage 2's world)")->envname("AOC_NO_COMBAT");
    app.add_option("--npc-aggro-cm", npc_aggro_cm, "how far (cm) an NPC whose data gives no aggro radius notices a player")->envname("AOC_NPC_AGGRO_CM")->capture_default_str();
    app.add_option("--npc-leash-cm", npc_leash_cm, "how far (cm) from home an NPC chases before it gives up and resets")->envname("AOC_NPC_LEASH_CM")->capture_default_str();
    // plan E1, stage 3, part 2: what follows a death (combat/aftermath.h)
    bool no_loot = false, no_respawn = false, no_death_loss = false;
    double corpse_seconds = 15.0, loot_corpse_seconds = 120.0, loot_chance_mult = 1.0;
    app.add_flag("--no-loot", no_loot, "the kill switch for what follows a death: no experience, no loot, no respawn (the corpse still goes)")->envname("AOC_NO_LOOT");
    app.add_flag("--no-death-loss", no_death_loss, "a player's death takes no materials (retail: about half of each stack, into a pile where it fell)")->envname("AOC_NO_DEATH_LOSS");
    app.add_flag("--no-respawn", no_respawn, "the kill switch for respawns: a dead NPC stays dead")->envname("AOC_NO_RESPAWN");
    app.add_option("--corpse-seconds", corpse_seconds, "how long a corpse with nothing to loot stays (retail: 15 s)")->envname("AOC_CORPSE_SECONDS")->capture_default_str();
    app.add_option("--loot-corpse-seconds", loot_corpse_seconds, "how long a corpse with loot that nobody takes stays (modelled: 120 s)")->envname("AOC_LOOT_CORPSE_SECONDS")->capture_default_str();
    app.add_option("--loot-chance-mult", loot_chance_mult, "multiplies the chance of every RollEachByPercent loot draw (never a quantity; each chance is capped at 1): 3 makes small drop chances 3 times as likely (1: the data's own)")
        ->envname("AOC_LOOT_CHANCE_MULT")->check(CLI::Range(0.0, 1000.0))->capture_default_str();
    // Gathering stage 2, "Harvesting": the resource nodes (gather/gathering.h)
    bool no_gathering = false, gather_quality_literal = false;
    double gather_respawn_mult = 1.0;
    app.add_flag("--no-gathering", no_gathering, "the kill switch for harvesting: the resource nodes are seen and not harvested")->envname("AOC_NO_GATHERING");
    app.add_option("--gather-respawn-mult", gather_respawn_mult,
                   "multiplies the time a gathered resource node takes to return (the data's: the actor def's OverrideRespawnDuration, else the foliage data's EmptyDuration; 0.02 turns a tree's 14,400 s into 288 s; at least 1 s)")
        ->envname("AOC_GATHER_RESPAWN_MULT")->check(CLI::Range(0.0001, 1000.0))->capture_default_str();
    app.add_flag("--gather-quality-literal", gather_quality_literal,
                 "GetRolledItemQuality() in the Quantity equation answers the literal quality (1, 6, 16 ...) instead of the index of its rarity band (0 to 6): the equation compares it with 0 to 6")
        ->envname("AOC_GATHER_QUALITY_LITERAL");
    // Gathering stage 3, "Foliage gathering": the painted trees, plants and stones (foliage/foliage.h); --no-gathering switches them off with the nodes
    double foliage_regrow_mult = 1.0;
    std::string foliage_map_path = "dist/content/region_foliage.tsv";
    app.add_option("--foliage-regrow-mult", foliage_regrow_mult,
                   "multiplies EVERY phase of a harvested tree's, plant's or stone's regrowth (the data's: an Oak large is 300 s empty, 1800 s a sapling, 1800 s medium, then whole; a plant 1800 s empty and 300 s a sapling; 0.01 makes the Oak 3 + 18 + 18 s)")
        ->envname("AOC_FOLIAGE_REGROW_MULT")->check(CLI::Range(0.0001, 1000.0))->capture_default_str();
    app.add_option("--foliage-map", foliage_map_path,
                   "the painted foliage of the area, dist/content/region_foliage.tsv (tools/mapc `mapc extract`: 84,218 instances of the Riverlands; a missing file: the foliage is seen and not harvested)")
        ->envname("AOC_FOLIAGE_MAP")->capture_default_str();
    app.add_option("--max-sessions", endpoint_config.max_sessions,
                   "the most client sessions at once; further handshakes are ignored (0: no limit)")
        ->capture_default_str();
    CLI11_PARSE(app, argc, argv);
// (C.Y.B | CoA)

    std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::stderr_color_sink_mt>()};
    if (!log_file.empty()) sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true));
    auto logger = std::make_shared<spdlog::logger>("worldd", sinks.begin(), sinks.end());
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);

    std::string error;
    const auto token = svc::load_service_token(token_file, &error);
    if (!token) {
        spdlog::error("{}", error);
        return 1;
    }
    // content.db: worldd runs without it but refuses logins (the Endpoint, with PlayerWorld::ready() false).
    std::optional<content::ContentDb> content = content::ContentDb::open(content_db_path, error);
    std::string content_problem;
    if (content) {
        spdlog::info("content.db {} opened", content_db_path);
    } else {
        content_problem = error;
        spdlog::error("content.db {} did not open: {}", content_db_path, content_problem);
        spdlog::error("worldd refuses every login until content.db is built (inputs and flags: services/README.md, \"Content\"); "
                      "restart worldd after building it");
    }
    const auto bind_addr = parse_address(bind);
    const auto public_addr = parse_address(public_address.empty() ? bind : public_address);
    if (!bind_addr || !public_addr || public_addr->ip == 0 || public_addr->port == 0) {
        spdlog::error("--bind and --public-address must be a.b.c.d:port, and the public address dialable");
        return 1;
    }
    // Bind before registering, so mesh never places players on a server that
    // cannot listen.
    auto socket = UdpSocket::open(*bind_addr, &error);
    if (!socket) {
        spdlog::error("{}", error);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    svc::GrpcServices services({gateway, character, mesh}, *token);
    svc::RegisterResult reg;
    for (auto backoff = std::chrono::milliseconds(500); !g_stop;) {
        reg = services.register_server(public_addr->to_string(), {});
        if (reg.status == svc::CallStatus::Ok) break;
        if (reg.status == svc::CallStatus::Refused) {
            spdlog::error("mesh refused to register {}", public_addr->to_string());
            return 1;
        }
        spdlog::warn("registering with mesh at {} failed ({}); retrying", mesh, svc::call_status_name(reg.status));
        sleep_unless_stopped(backoff);
        backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
    }
    if (g_stop) return 0;
    spdlog::info("worldd {} registered with mesh as server {}", public_addr->to_string(), reg.server_id);

    uint32_t randomizer = 0;
    while (randomizer == 0) secure_random(reinterpret_cast<uint8_t*>(&randomizer), sizeof(randomizer));
    aoc::protocol::ServerGuidAllocator guids(reg.server_id, randomizer);
// -- C | O | A . C_Y_B --

    const auto mode = gamestate_mode == "captured" ? bootstrap::GameStateMode::Captured
                      : gamestate_mode == "tier3"  ? bootstrap::GameStateMode::Tier3
                      : gamestate_mode == "tier2"  ? bootstrap::GameStateMode::Tier2
                                                   : bootstrap::GameStateMode::Tier1;
    spdlog::info("GameState mode {}", gamestate_mode);
    // The positions of players go to the character service on one worker thread. It is declared before the endpoint, so it
    // outlives every session and is flushed (bounded) when worldd stops: once, by the flush below (the destructor does not
    // wait again; it only abandons what is left and lets the call in flight, at most GrpcServices' save deadline, end).
    // Nodes 1, stage 1: the city node registry.  Declared before the world that points at it, so it is destroyed after it.
    std::unique_ptr<world::CityNodes> city_nodes;
    // Nodes 1, stage 2: what the client is told about the nodes, and the node GM console's door to the node service (both outlive the world that points at them)
    std::unique_ptr<world::NodeWire> node_wire;
    std::unique_ptr<world::NodeLayoutData> node_layout_data;
    std::unique_ptr<spawns::NodePopulationData> node_pop_data;
    std::unique_ptr<svc::GrpcNodeControl> node_control;
    CharacterSaver::Config saver_config;
    saver_config.destructor_wait = std::chrono::milliseconds(0);
    CharacterSaver saver(services, saver_config);
    PlayerWorld players(content ? &*content : nullptr, content_problem, mode, reg.server_id, guids);
    if (content && !players.ready()) {
        // content.db opened, but its ability rows are not what this worldd runs (plan A7): the same refusal as a missing file
        spdlog::error("worldd refuses every login: {}", players.content_problem());
    }
    players.set_movement(movement);
    players.set_position_sink(&saver);
    players.set_gm_commands(gm_commands);
    spdlog::info("[gm] the client's own GM teleport commands are {}", gm_commands ? "ON (--gm-commands)" : "off (--gm-commands, or AOC_GM_COMMANDS=1, turns them on); each one the client sends is logged");
    // Nodes 1, stage 1: the registry from content.db, the seed's levels at once (login never waits for the service), then the stream.  The feed is declared
    // after the world, so it stops first.
    std::unique_ptr<svc::CityNodeFeed> city_feed;
    if (content && players.ready()) {
        std::string nodes_error;
        city_nodes = world::CityNodes::load(content_db_path, nodes_error);
        if (!city_nodes) {
            spdlog::error("[nodes] no city nodes ({}): the loot tables' node reads answer as in a world with no nodes", nodes_error);
        } else {
            size_t applied = 0, skipped = 0;
            std::string seed_error;
            if (city_nodes->apply_seed_file(citynode_seed, seed_error, &applied, &skipped)) {
                spdlog::info("[nodes] {} city nodes, {} zones of influence; the seed {} gives {} levels{}: {}", city_nodes->size(), city_nodes->zones(), citynode_seed, applied,
                             skipped > 0 ? " (" + std::to_string(skipped) + " entries skipped)" : "", city_nodes->describe_levels());
            } else {
                spdlog::warn("[nodes] {} city nodes, {} zones of influence; no seed levels ({}): every node is at Wilderness until the citynode service answers", city_nodes->size(),
                             city_nodes->zones(), seed_error);
            }
            players.set_city_nodes(city_nodes.get());
            std::string wire_error;
            node_wire = world::NodeWire::load(content_db_path, *city_nodes, wire_error);
            if (!node_wire) {
                spdlog::error("[nodes] no node maps for the client ({}): the GameState goes without them", wire_error);
            } else {
                players.set_node_wire(node_wire.get());
                spdlog::info("[nodes] the client is told of {} nodes (retail's list), {} layouts' plots and {} estates; GameState mode {}", node_wire->roster().size(),
                             node_wire->layouts(), node_wire->estates(), gamestate_mode);
                if (!no_node_people) {
                    std::string people_error;
                    node_pop_data = spawns::NodePopulationData::load(content_db_path, people_error);
                    if (!node_pop_data) {
                        spdlog::error("[people] no town people ({}): the towns have no NPCs", people_error);
                    } else {
                        players.set_node_population_data(node_pop_data.get());
                        spdlog::info("[people] {} layouts with population sets, {} buildings with populations", node_pop_data->layouts(), node_pop_data->buildings());
                    }
                } else {
                    spdlog::info("[people] --no-node-people: no town people");
                }
                if (!no_node_scenery) {
                    std::string layout_error;
                    node_layout_data = world::NodeLayoutData::load(content_db_path, layout_error);
                    if (!node_layout_data) {
                        spdlog::error("[scenery] no town scenery ({}): the towns are empty holes", layout_error);
                    } else {
                        players.set_node_layout_data(node_layout_data.get());
                        spdlog::info("[scenery] {} layouts, {} asset sets, {} asset definitions, {} buildings", node_layout_data->layouts(), node_layout_data->asset_sets(),
                                     node_layout_data->definitions(), node_layout_data->buildings());
                    }
                } else {
                    spdlog::info("[scenery] --no-node-scenery: no town replicators");
                }
            }
            if (!no_citynode) {
                node_control = std::make_unique<svc::GrpcNodeControl>(svc::GrpcNodeControl::Options{citynode, *token});
                players.set_node_control(node_control.get());
            }
            if (no_citynode) {
                spdlog::info("[nodes] --no-citynode: the nodes stay at the seed's levels");
            } else {
                city_feed = std::make_unique<svc::CityNodeFeed>(*city_nodes, svc::CityNodeFeed::Options{citynode, *token});
                city_feed->start();
            }
        }
    }
    if (players.ready() && area_radius_m != 0) {
        if (area_radius_m < 0) {
            area = spawns::AreaConfig::riverlands();
            area.zones = !no_zone_filter;
        }
        else area.radius_cm = area_radius_m * 100.0;
        players.set_gm_garden(gm_garden);
        std::string area_error;
        const auto started = std::chrono::steady_clock::now();
        {
            spawns::GenericConfig generic;
            generic.enabled = !no_generic_npcs;
            generic.hunting = !no_gathering;   // Gathering stage 3: with harvesting off a creature is not huntable (no hunting interactable)
            for (size_t start = 0; start <= npc_exclude.size();) {
                const size_t comma = npc_exclude.find(',', start);
                std::string one = npc_exclude.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                while (!one.empty() && std::isspace(static_cast<unsigned char>(one.front()))) one.erase(one.begin());
                while (!one.empty() && std::isspace(static_cast<unsigned char>(one.back()))) one.pop_back();
                if (!one.empty()) generic.exclude.push_back(one);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            players.set_generic_npcs(std::move(generic));
            spawns::SpawnContext context;
            context.enabled = !no_spawn_filter;
            context.humanoids = spawn_humanoids;
            context.node_level = spawn_node_level;
            if (spawns::SpawnFilter::node_level_number(spawn_node_level) < 0 || spawns::SpawnFilter::node_level_number(spawn_node_level) > 6) {
                spdlog::warn("[area] --spawn-node-level {} is no node level: Wilderness is used", spawn_node_level);
                context.node_level = "Wilderness";
            }
            if (spawn_hour >= 0) {
                context.hour = spawn_hour;
            } else {
                const std::time_t t = std::time(nullptr);
                std::tm local{};
#ifdef _WIN32
                localtime_s(&local, &t);
#else
                localtime_r(&t, &local);
#endif
                context.hour = local.tm_hour + local.tm_min / 60.0;
            }
            players.set_spawn_context(std::move(context));
            combat::CombatWorldConfig combat_config;
            combat_config.enabled = !no_combat;
            combat_config.default_aggro_cm = npc_aggro_cm;
            combat_config.default_leash_cm = npc_leash_cm;
            players.set_combat_config(std::move(combat_config));
            combat::AftermathConfig aftermath;
            aftermath.enabled = !no_loot;
            aftermath.respawn = !no_respawn;
            aftermath.death_loss = !no_death_loss;
            aftermath.corpse_seconds = corpse_seconds;
            aftermath.loot_corpse_seconds = loot_corpse_seconds;
            aftermath.loot_chance_mult = loot_chance_mult;
            players.set_aftermath_config(std::move(aftermath));
            std::string table_error;
            if (!players.load_blueprints(npc_motion_path, table_error)) {
                spdlog::warn("[area] no Blueprint table ({}): the NPCs no retail capture opened cannot be made (run tools/mapc: mapc build)", table_error);
            }
        }
        {
            std::string speeds_error;
            if (players.mounts() && !players.load_mount_speeds(mount_speeds_path, speeds_error)) {
                spdlog::warn("[content] no mount speeds ({}): a mount runs at 600 cm/s times its item's ground percentage when it is desummoned (run tools/mapc: mapc mount-speeds)", speeds_error);
            }
        }
        {
            std::string riders_error;
            if (players.mounts() && !players.load_mount_riders(mount_riders_path, riders_error)) {
                spdlog::warn("[content] no mount riders ({}): each mount names the rider AnimBP of its kind's retail donor (run tools/mapc: mapc mount-speeds)", riders_error);
            }
        }
        {
            std::string roads_error;
            // Nodes 1, stage 3b: the roads of every node level with their owner zones, when the file is there (contentc roads --levels): each road's tier follows its zone's node
            const bool by_level = std::filesystem::exists(roads_levels_path);
            players.set_road_default_level(std::max(0, spawns::SpawnFilter::node_level_number(spawn_node_level)));
            if (by_level) roads_path = roads_levels_path;
            else if (players.mounts()) {   // (a build copied without dist/content/roads_levels.tsv would otherwise fall back to roads.tsv without a word; Nodes 1 review B)
                spdlog::warn("[content] no {} (contentc roads --levels writes it): the roads keep the tiers of {} and do not follow the city nodes' levels", roads_levels_path, roads_path);
            }
            if (players.mounts() && !players.load_roads(roads_path, roads_error)) {
                spdlog::warn("[content] no roads ({}): a mount rides at its own speed everywhere (run tools/mapc: mapc roads, then contentc roads)", roads_error);
            } else if (players.mounts() && players.roads().node_level() >= 0 && players.roads().node_level() != spawns::SpawnFilter::node_level_number(spawn_node_level)) {
                spdlog::warn("[content] {} resolved the roads' tiers at node level {}, the world runs at {} ({}): run contentc roads --node-level {}", roads_path, players.roads().node_level(),
                             spawns::SpawnFilter::node_level_number(spawn_node_level), spawn_node_level, spawn_node_level);
            }
        }
        if (!players.load_area(area, area_error)) {
            spdlog::error("the living area is not loaded (players see no other actors): {}", area_error);
        } else {
            spdlog::info("the living area loaded in {} ms: {} actors", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(),
                         players.registry().size());
            if (no_npc_movement) {
                spdlog::info("[nav] --no-npc-movement: the NPCs stand where the data puts them");
            } else {
                std::string ground_error;
                if (!players.load_ground(area_nav_path, npc_motion_path, ground_error)) {
                    spdlog::error("[nav] no ground, the NPCs stand where the data puts them and do not move: {} (build it: dotnet tools/mapc/bin/Release/net8.0/mapc.dll build)", ground_error);
                }
            }
        }
        // the GM garden needs the ground (load_ground places it); without a ground file it stands at the login spot's height
        if (players.area_report() && players.gm_garden_requested() && !players.garden_report()) players.place_gm_garden();
        // Nodes 1, stage 3a: the towns' scenery, once the node levels are known (the seed's, and the service's as they come)
        {
            world::NodeScenery::Config scenery;
            scenery.enabled = !no_node_scenery;
            scenery.captured_origins = node_origin != "record";
            scenery.awake_cm = scenery_radius_m * 100.0;
            players.start_scenery(scenery);
        }
        // Nodes 1, stage 3b: the towns' people, once the area, the ground and the node levels are known
        {
            spawns::NodePopulations::Config people;
            people.enabled = !no_node_people;
            players.start_node_people(people);
        }
        // Gathering stage 2, "Harvesting": the nodes of the area can be harvested
        if (no_gathering) {
            spdlog::info("[gather] --no-gathering: the resource nodes are seen and not harvested");
        } else {
            gather::Config gathering;
            gathering.board.respawn_mult = gather_respawn_mult;
            gathering.quality_literal = gather_quality_literal;
            std::string gather_error;
            if (!players.start_gathering(gathering, gather_error)) {
                spdlog::warn("[gather] harvesting is off: {} (the resource nodes are seen and not harvested)", gather_error);
            } else {
                foliage::Config trees;
                trees.map_path = foliage_map_path;
                trees.regrow_mult = foliage_regrow_mult;
                std::string foliage_error;
                if (!players.start_foliage(trees, foliage_error)) spdlog::warn("[foliage] harvesting is off: {} (the trees, plants and stones of the world are seen and not harvested)", foliage_error);
            }
        }
    } else if (area_radius_m == 0) {
        spdlog::info("--area-radius 0: players see no other actors");
    }
    spdlog::info("moves are checked: at most {:.0f} cm/s along the ground; positions are saved every {} s and when a session ends",
                 movement.limits.max_speed, movement.save_interval.count());
    endpoint_config.players = &players;
// C-Y-B and C-O-A

    std::atomic<int> exit_code{0};
    std::thread heartbeat([&] {
        const auto interval = std::chrono::milliseconds(std::max<int64_t>(reg.heartbeat_interval_ms, 100));
        for (;;) {
            sleep_unless_stopped(interval);
            if (g_stop) return;
            const auto s = services.heartbeat(reg.server_id);
            if (s == svc::CallStatus::NotFound) {
                // The registry forgot this server: its ServerId (and every
                // NetGUID minted under it) is dead.  Exit; do not re-register.
                spdlog::error("mesh no longer knows server {}; stopping", reg.server_id);
                exit_code = 1;
                g_stop = true;
                return;
            }
            if (s != svc::CallStatus::Ok) spdlog::warn("heartbeat to mesh failed ({})", svc::call_status_name(s));
        }
    });

    CookieSigner signer;
    LoginService login(services, services);
    Endpoint* endpoint_ptr = nullptr;
    LoginWorker worker(login, reg.server_id, [&](uint64_t id, LoginResult r) {
        endpoint_ptr->post_login_result(id, std::move(r));
    });
    Endpoint endpoint(endpoint_config, signer, worker,
                      [&](const Address& to, const std::vector<uint8_t>& pkt) { socket->send(to, pkt.data(), pkt.size()); },
                      secure_random, Clock::now());
    endpoint_ptr = &endpoint;

    // the GM travel tools: the admin API's calls are applied on this thread (the loop below drains the queue), where the sessions live
    GmTravel travel(players, [&] { return endpoint.players(); }, [] { return Clock::now(); });
    players.set_gm_port(&travel);
    svc::AdminQueue admin_queue;
    std::unique_ptr<svc::WorldAdminServer> admin;
    if (no_world_admin) {
        spdlog::info("[admin] --no-world-admin: the admin API is not served");
    } else {
        admin = std::make_unique<svc::WorldAdminServer>(travel, admin_queue, svc::WorldAdminServer::Options{world_admin, *token});
        std::string admin_error;
        if (admin->start(&admin_error)) {
            const size_t places = players.places().all().size();
            const bool loopback = world_admin.rfind("127.", 0) == 0 || world_admin.rfind("localhost", 0) == 0 || world_admin.rfind("[::1]", 0) == 0;
            spdlog::info("[admin] the admin API listens on {} ({} places to teleport to; worldctl players | places | tp | speed){}", world_admin, places,
                         loopback ? "" : "; NOT loopback: anyone who can reach it and has the service token can move players");
        } else {
            spdlog::error("[admin] the admin API is not served: {}", admin_error);
            admin.reset();
        }
    }

    std::vector<uint8_t> buf(4096);
    // The exception barrier: the Endpoint drops a session whose own handling throws; anything that gets past it
    // (the handshake, a login result) is logged and the loop goes on, so one fault never ends every player's session.
    size_t tick_errors = 0;
    const auto guarded_datagram = [&](const Address& from, size_t n, Clock::time_point now) {
        try {
            endpoint.on_datagram(from, buf.data(), n, now);
        } catch (const std::exception& e) {
            spdlog::error("a datagram from {} threw {}: dropping its session, if any", from.to_string(), e.what());
            endpoint.drop(from, "threw an exception");
        }
    };
    const auto guarded_tick = [&](Clock::time_point now) {
        try {
            endpoint.tick(now);
            tick_errors = 0;
        } catch (const std::exception& e) {
            if (++tick_errors == 1 || tick_errors % 1000 == 0) spdlog::error("the tick threw {} ({} in a row): going on", e.what(), tick_errors);
        }
    };

    spdlog::info("listening on UDP {}", socket->local_address().to_string());
    while (!g_stop) {
        Address from;
        const auto n = socket->receive(buf.data(), buf.size(), from, std::chrono::milliseconds(10));
        const auto now = Clock::now();
        if (!n) {
            spdlog::error("the UDP socket failed; stopping");
            exit_code = 1;
            break;
        }
        if (*n > 0) guarded_datagram(from, *n, now);
        guarded_tick(now);
        admin_queue.drain();
    }

    if (admin) admin->stop();
    players.set_gm_port(nullptr);
    spdlog::info("stopping: telling {} connected clients", endpoint.session_count());
    g_stop = true;
    endpoint.kick_all(login_text::kShuttingDown, Clock::now());
    for (const auto until = Clock::now() + std::chrono::milliseconds(500); Clock::now() < until;) {
        Address from;
        const auto n = socket->receive(buf.data(), buf.size(), from, std::chrono::milliseconds(10));
        if (n && *n > 0) guarded_datagram(from, *n, Clock::now());
        guarded_tick(Clock::now());
    }
    // Players still on the endpoint have their last good position handed to the saver, then the saver gets 5 s. Worst case
    // from the signal to the exit: 0.5 s telling the clients, 5 s here, then the save in flight when the saver is
    // destroyed (2 s deadline): 7.5 s. The network thread is never in the saver's way: it only queues.
    endpoint.finish_players("worldd is stopping");
    if (!saver.flush(std::chrono::seconds(5))) {
        spdlog::warn("stopping: {} position(s) were not saved in 5 s", saver.queued());
    }
    heartbeat.join();
    return exit_code;
}

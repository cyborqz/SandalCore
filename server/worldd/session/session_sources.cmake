# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
# ============================================================================
#  session/session_sources.cmake: the source files of the player session's features (included by worldd/CMakeLists.txt)
#
#  PlayerSession is one class (session/player_session.h); each feature defines its members in a file of its own, like player_gather.cpp.  A new feature adds its file to this
#  list, and one line to session/rpc_registered.inc when it answers client calls (session/rpc_table.h).
#
#  The list is a file of its own and part of worldd_core only.  The navigation mesh builder (aoc_navbuild) links worldd_nav, which holds none of these files, so a line added
#  here does not make the extractor build the mesh again.  The pets' combat files are listed here for the same reason.
# ============================================================================
target_sources(worldd_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap/observer_actor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/combat/combat_pets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/combat/combat_world_npc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/net/endpoint_phases.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/quest/session_givers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/quest/session_handin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/quest/session_objects.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/quest/session_quests2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/respawn/session_embersprings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/ability_pipeline_pets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_abilities.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_based_move.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_chat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_chests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_combat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_combat_side.h
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_craft.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_dialogue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_ignore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_emotes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_interact.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_items.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_mounts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_move_ack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_movement.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_citizenship.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_citizenship_gm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_node_taxes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_node_inventory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_nodes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_pets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_presence.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_processing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_progress.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_quests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_requests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_session_util.h
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_talents.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_vendors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/player_view_phases.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/rpc_table.h
    ${CMAKE_CURRENT_SOURCE_DIR}/session/shared_updates.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/shared_updates.h
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view_pool.h
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view_worker.h
    ${CMAKE_CURRENT_SOURCE_DIR}/warehouse/player_warehouse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/citizenship.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/citizenship.h
    ${CMAKE_CURRENT_SOURCE_DIR}/world/elections.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/elections.h
)

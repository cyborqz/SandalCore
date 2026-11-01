// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/rpc_registered.inc: the features of PlayerSession that register client calls (see session/rpc_table.h)
//  One line per feature: AOC_RPC_FEATURE(<feature>) names the function PlayerSession::register_<feature>_rpcs(RpcTable&), which the feature defines in its own file.  A new
//  feature adds its line at the END. Two branches that add a line at the same time both keep it (the merge=union rule of .gitattributes), so this list does not conflict.  The
//  order of the lines does not matter.
// ============================================================================
AOC_RPC_FEATURE(core)
AOC_RPC_FEATURE(combat)
AOC_RPC_FEATURE(abilities)
AOC_RPC_FEATURE(mounts)
AOC_RPC_FEATURE(movement)
AOC_RPC_FEATURE(items)
AOC_RPC_FEATURE(talents)
AOC_RPC_FEATURE(requests)
AOC_RPC_FEATURE(interact)
AOC_RPC_FEATURE(vendors)
AOC_RPC_FEATURE(craft)
AOC_RPC_FEATURE(processing)
AOC_RPC_FEATURE(dialogue)
AOC_RPC_FEATURE(quests)
AOC_RPC_FEATURE(nodes)
AOC_RPC_FEATURE(warehouse)
AOC_RPC_FEATURE(respawn)
AOC_RPC_FEATURE(handin)
AOC_RPC_FEATURE(gather)
AOC_RPC_FEATURE(foliage)

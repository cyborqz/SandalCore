// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  worldd_pch.h: the precompiled header of worldd_core (server/worldd/CMakeLists.txt, target_precompile_headers).
//
//  What is here is read once for the library, and every file of it starts from the parsed state instead of reading the same headers again.  Put a header here only when
//  both hold: more than half of the library's files include it, and it hardly ever changes.  A change to any header in this file builds the precompiled header again and
//  then every file of the library; the headers that change often (content_db.h, combat_world.h, ability_pipeline.h, player_session.h and its members, player_world.h, the
//  generated headers) are therefore not here, and neither are the headers of the other targets' own tables.
//
//  The files are compiled with this header read first (/FI), so a file that must define a macro before a standard header cannot be a file of this library: the definitions
//  of the library (NOMINMAX, WIN32_LEAN_AND_MEAN ...) are options of the target and are the same for the precompiled header.
// ============================================================================
#pragma once

// the standard library headers that most files of the library include
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// the log every part of the server prints through
#include <spdlog/spdlog.h>

// libs/aocnet: the wire primitives, the emit side and the replication headers (unchanged in all but one of the last 57 commits of main)
#include "protocol/wire/ue5_primitives.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/wire/packet_reader.h"
#include "protocol/repl/value.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/subobject.h"
#include "protocol/repl/rep_state.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/replicator.h"

// the stat engine and the rarity table (unchanged in the last 57 commits of main)
#include "items/rarity.h"
#include "stats/gear.h"
#include "stats/character_stats.h"
#include "stats/stat_engine.h"
#include "stats/stat_program.h"

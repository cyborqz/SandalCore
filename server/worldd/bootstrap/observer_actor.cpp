// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Players seeing each other (stage A): written by unspike in #14.
// Moved here from bootstrap/bootstrap.cpp, where the PlayerBootstrap builders of the other files of this folder stay, so that the feature is a file of its own (CONTRIBUTING.md,
// "Where New Code Goes").
// ============================================================================
//  bootstrap/observer_actor.cpp  (players seeing each other, stage A)
//
//  The player's pawn as the OTHER players are shown it (PlayerBootstrap::observer_pawn, declared in bootstrap/bootstrap.h): the owner's pawn with only the values and components that
//  show the character.  The session puts it into the world's registry once the phase-in is built (session/player_presence.cpp).
//
//  #14 review: the PlayerState that was built beside the pawn (observer_player_state) is gone, and the pawn names none: the other players are shown the pawn alone, and nothing of the
//  login text (the ?Name= option, which names the player's PC) reaches their clients.
// ============================================================================
#include "bootstrap/bootstrap.h"

#include "bootstrap/constants.h"
#include "protocol/repl/move_packed.h"
#include "protocol/schema/generated/index.h"

#include <initializer_list>
#include <string_view>
#include <vector>

namespace aoc::worldd::bootstrap {

using protocol::emit::FIntrepidNetworkGUID;
using repl::Value;
namespace schema = ::aoc::protocol::schema;

namespace {

Value object(const FIntrepidNetworkGUID& g) { return Value(g); }

/// A component of the owner's pawn as another player is shown it: the same GUID, name and checksum (or dynamic class), with only `paths` of its values and only
/// `arrays` of its fast arrays (their items and keys as they are).  False when a value or an item does not fit (never: they came from the same class).
bool observer_component(const repl::Subobject& from, const std::vector<std::string_view>& paths, const std::vector<std::string_view>& arrays, repl::Subobject& out) {
    out = repl::Subobject{};
    out.guid = from.guid;
    out.name = from.name;
    out.checksum = from.checksum;
    out.dynamic_class = from.dynamic_class;
    out.outer = from.outer;
    out.obj.schema = from.obj.schema;
    out.obj.opts = from.obj.opts;
    if (!from.obj.schema) return false;
    const schema::ClassSchema& cls = *from.obj.schema;
    for (const std::string_view path : paths) {
        const Value* v = from.obj.state.get(cls, path);
        if (v && !out.obj.state.set(cls, path, *v)) return false;   // (a value the owner's pawn never set stays the class default)
    }
    repl::FastArrays source = from.obj.arrays;   // (FastArrays::get creates an array on first use: a copy, to read it)
    for (const std::string_view name : arrays) {
        const repl::FastArrayState* src = source.get(cls, name);
        repl::FastArrayState* dst = out.obj.arrays.get(cls, name);
        if (!src || !dst) return false;
        for (const repl::FastArrayItem& it : src->items()) {
            if (!dst->insert(it.id, it.values)) return false;
        }
        dst->set_array_key(src->array_key());
    }
    return true;
}

}  // namespace

std::optional<repl::Actor> PlayerBootstrap::observer_pawn() const {
    if (!ok_ || !second_state_added_ || !pawn_.obj.schema) return std::nullopt;
    const SessionGuids& g = setup_.guids;
    repl::Actor a;
    a.guid = pawn_.guid;
    a.obj.schema = pawn_.obj.schema;
    a.obj.opts = pawn_.obj.opts;
    a.open = pawn_.open;   // the archetype, the level, where it spawned (the caller's write_motion puts it where the player is)
    bool ok = true;
    const auto set = [&](std::string_view path, Value v) { ok = a.obj.state.set(*a.obj.schema, path, std::move(v)) && ok; };
    set("AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated));
    set("RemoteRole", Value::u64(1));   // SimulatedProxy: what every connection but the owner's is
    set("Role", Value::u64(4));         // Authority
    set("Instigator", object(g.pawn));
    // #14 review: no PlayerState is set (the owner's pawn names its own PlayerState, whose PlayerNamePrivate is the login's ?Name= text, chosen by the client); the pawn names none
    set("ReplicatedMovementMode", Value::u64(repl::kMoveWalking));
    // the components that show the character, in the owner's block order (Alignment, BaseCharacterInfo, CombatInfo, Stats, Appearance)
    struct Want {
        const FIntrepidNetworkGUID*            guid;
        std::initializer_list<std::string_view> paths, arrays;
    };
    const Want wants[] = {
        {&g.alignment, {"Alignments"}, {}},
        {&g.base_character_info,
         {"CharacterGuid.A", "CharacterGuid.B", "CharacterGuid.C", "CharacterGuid.D", "CharacterName", "Race", "Gender", "Level", "ServerId", "bHasBeenPossessed"},
         {"HeldItems"}},
        {&g.combat_info, {"bIsDead", "PrimaryClassId.Guid"}, {}},
        {&g.stats, {}, {"StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}},
        {&g.appearance, {"SharedAppearanceInfoId.Guid", "CharacterCustomization", "AppearanceIDs"}, {}},
    };
    for (const Want& w : wants) {
        const repl::Subobject* from = pawn_.find(*w.guid);
        if (!from) return std::nullopt;
        repl::Subobject sub;
        if (!observer_component(*from, w.paths, w.arrays, sub)) return std::nullopt;
        a.subobjects.push_back(std::move(sub));
    }
    if (!ok) return std::nullopt;
    return a;
}

}  // namespace aoc::worldd::bootstrap

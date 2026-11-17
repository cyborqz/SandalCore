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
//  show the character, and an AbilityComponent that shows the emotes it plays (observer_ability_component).  The session puts it into the world's registry once the phase-in is built
//  (session/player_presence.cpp).
//
//  #14 review: the PlayerState that was built beside the pawn (observer_player_state) is gone, and the pawn names none: the other players are shown the pawn alone, and nothing of the
//  login text (the ?Name= option, which names the player's PC) reaches their clients.
// ============================================================================
#include "bootstrap/bootstrap.h"

#include "bootstrap/constants.h"
#include "content/alignment_ids.h"
#include "content/emote_commands.h"
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

/// The AbilityComponent as another player is shown it, for the emotes: it holds no ability item (the session puts the emote items of the owner's pawn in, session/player_emotes.cpp) and asks
/// the other player's client to load the records of the emote abilities the owner's component loads, which it needs to play them.  The owner's KnownAbilities, its Cooldowns and the records
/// of its class's abilities stay the owner's.  False when the empty array does not exist.
bool observer_ability_component(const repl::Subobject& from, repl::Subobject& out) {
    const schema::ClassSchema& cls = *from.obj.schema;
    const Value* records = from.obj.state.get(cls, "ReplicatedRecordsToLoad");
    const auto* list = records ? records->get_if<repl::ArrayValue>() : nullptr;
    if (list) {
        repl::ArrayValue shown;
        for (const repl::ElementState& e : list->elements) {
            const auto guid = e.values.find(1);
            if (guid != e.values.end() && guid->second.get_if<int64_t>() && content::is_emote_ability(*guid->second.get_if<int64_t>())) shown.elements.push_back(e);
        }
        if (!out.obj.state.set(cls, "ReplicatedRecordsToLoad", Value(std::move(shown)))) return false;
    }
    return out.obj.arrays.get(cls, "ReplicatedActiveAbilities") != nullptr;
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
        {&g.combat_info,
         {"bIsTargeting", "bIsDead", "bInCombat", "bPvPForceFlag", "InPvpSuppressionArea", "bInPVPCombat", "PrimaryClassId.Guid", "bIsUsingRangedWeapon"},
         {}},
        {&g.ability, {}, {}},   // the emotes: observer_ability_component
        {&g.stats, {}, {"StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}},
        {&g.appearance, {"SharedAppearanceInfoId.Guid", "CharacterCustomization", "AppearanceIDs"}, {}},
    };
    for (const Want& w : wants) {
        const repl::Subobject* from = pawn_.find(*w.guid);
        if (!from) return std::nullopt;
        repl::Subobject sub;
        if (!observer_component(*from, w.paths, w.arrays, sub)) return std::nullopt;
        if (w.guid == &g.ability && !observer_ability_component(*from, sub)) return std::nullopt;
        if (w.guid == &g.combat_info) {
            // the stance bits go out even when the player's pawn never set one, so another client's copy starts from the game's own defaults (0) and the bits are current in the open
            for (const char* path : {"bIsTargeting", "bInCombat", "bPvPForceFlag", "InPvpSuppressionArea", "bInPVPCombat"}) {
                if (sub.obj.state.get(*sub.obj.schema, path)) continue;
                if (!sub.obj.state.set(*sub.obj.schema, path, Value::boolean(false))) return std::nullopt;
            }
        }
        a.subobjects.push_back(std::move(sub));
    }
    if (!ok) return std::nullopt;
    return a;
}

// ---- gear, weapons and level, kept current on the pawn the other players are shown (Social, seeing other players: gear and level changes) -------------

namespace {
repl::FastArrayState* observer_array(repl::Subobject& sub, std::string_view name) {
    if (!sub.obj.schema) return nullptr;
    return sub.obj.arrays.get(*sub.obj.schema, name);
}
const repl::FastArrayState* observer_array(const repl::Subobject& sub, std::string_view name) {
    if (!sub.obj.schema) return nullptr;
    const schema::NetField* f = sub.obj.schema->field(name);
    return f ? sub.obj.arrays.find(f->index) : nullptr;
}
/// The item of the stat `record` in an EveryoneProxy array (nullptr: the array has none).
const repl::FastArrayItem* observer_stat_item(const repl::FastArrayState& array, int64_t record) {
    for (const repl::FastArrayItem& it : array.items()) {
        const auto found = it.values.values.find(1);
        const auto* t = found != it.values.values.end() ? found->second.get_if<repl::StatInt32Inst>() : nullptr;
        if (t && t->record == record) return &it;
    }
    return nullptr;
}
}  // namespace

bool PlayerBootstrap::set_observer_gear(repl::Actor& shown, std::span<const HeldItem> held, std::span<const repl::AppearanceId> appearance, size_t marks) {
    if (!ok_ || !second_state_added_ || marks == 0 || marks > 64) return false;
    repl::Subobject* base = shown.find(setup_.guids.base_character_info);
    repl::Subobject* app = shown.find(setup_.guids.appearance);
    if (!base || !app) return false;
    repl::FastArrayState* array = observer_array(*base, "HeldItems");
    if (!array || !app->obj.schema) return false;
    const auto state_of = [](const HeldItem& h) {
        repl::ElementState e;
        e.values[1] = Value::u64(h.slot);
        e.values[2] = Value::i64(h.item_guid);
        e.values[3] = Value::u64(h.enchantment);
        return e;
    };
    // the first items of a shown pawn that never had a list: an insert alone replicates nothing, so the array is marked once
    const bool first = array->items().empty();
    if (first) {
        for (const HeldItem& h : held) {
            if (!array->insert(h.id, state_of(h))) return false;
        }
        array->mark_array_dirty();
    } else {
        for (size_t m = 0; m < marks; ++m) {
            for (const HeldItem& h : held) {
                if (!array->change(h.id, state_of(h))) return false;
            }
        }
    }
    repl::ArrayValue ids;
    for (const repl::AppearanceId& id : appearance) {
        repl::ElementState e;
        e.values[1] = Value(id);
        ids.elements.push_back(std::move(e));
    }
    return app->obj.state.set(*app->obj.schema, "AppearanceIDs", Value(std::move(ids)));
}

bool PlayerBootstrap::set_observer_level(repl::Actor& shown, int level) {
    if (!ok_ || !second_state_added_ || level < 1 || level > 255) return false;
    repl::Subobject* base = shown.find(setup_.guids.base_character_info);
    if (!base || !base->obj.schema) return false;
    return base->obj.state.set(*base->obj.schema, "Level", Value::i64(level));
}

bool PlayerBootstrap::set_observer_ranged(repl::Actor& shown, bool ranged) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* combat = shown.find(setup_.guids.combat_info);
    if (!combat || !combat->obj.schema || !combat->obj.schema->command("bIsUsingRangedWeapon")) return false;
    return combat->obj.state.set(*combat->obj.schema, "bIsUsingRangedWeapon", Value::boolean(ranged));
}

bool PlayerBootstrap::set_observer_dead(repl::Actor& shown, bool dead) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* combat = shown.find(setup_.guids.combat_info);
    if (!combat || !combat->obj.schema || !combat->obj.schema->command("bIsDead")) return false;
    return combat->obj.state.set(*combat->obj.schema, "bIsDead", Value::boolean(dead));
}

bool PlayerBootstrap::set_observer_stance(repl::Actor& shown, const ObserverStance& stance) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* combat = shown.find(setup_.guids.combat_info);
    if (!combat || !combat->obj.schema) return false;
    const schema::ClassSchema& cls = *combat->obj.schema;
    for (const char* path : {"bIsTargeting", "bInCombat", "bPvPForceFlag", "InPvpSuppressionArea", "bInPVPCombat"}) {
        if (!cls.command(path)) return false;
    }
    bool ok = true;
    ok = combat->obj.state.set(cls, "bIsTargeting", Value::boolean(stance.targeting)) && ok;
    ok = combat->obj.state.set(cls, "bInCombat", Value::boolean(stance.in_combat)) && ok;
    ok = combat->obj.state.set(cls, "bPvPForceFlag", Value::boolean(stance.pvp_flag)) && ok;
    ok = combat->obj.state.set(cls, "InPvpSuppressionArea", Value::boolean(stance.suppression)) && ok;
    ok = combat->obj.state.set(cls, "bInPVPCombat", Value::boolean(stance.pvp_combat)) && ok;
    return ok;
}

bool PlayerBootstrap::set_observer_alignments(repl::Actor& shown) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* dst = shown.find(setup_.guids.alignment);
    const repl::Subobject* src = pawn_.find(setup_.guids.alignment);
    if (!dst || !src || !dst->obj.schema || !src->obj.schema) return false;
    const repl::Value* held = src->obj.state.get(*src->obj.schema, "Alignments");
    if (!held) return false;
    return dst->obj.state.set(*dst->obj.schema, "Alignments", *held);
}

// ---- the player's own pawn: the draw and the PvP flag's name (Social, seeing other players: their combat stances) -------------------------------------
// (these write the player's own pawn, as set_in_combat does; the pawn the other players are shown takes them from there, session/player_stance.inc)

static_assert(static_cast<uint64_t>(content::kAlignmentNonCombatant) == kAlignmentKeyB, "the join's second alignment is the NonCombatant");

bool PlayerBootstrap::set_targeting(bool targeting) {
    if (!ok_) return false;
    repl::Subobject* combat = component(setup_.guids.combat_info);
    if (!combat || !combat->obj.schema->command("bIsTargeting")) return false;
    return combat->obj.state.set(*combat->obj.schema, "bIsTargeting", Value::boolean(targeting));
}

bool PlayerBootstrap::set_observer_stats(repl::Actor& shown) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* dst = shown.find(setup_.guids.stats);
    const repl::Subobject* src = pawn_.find(setup_.guids.stats);
    if (!dst || !src || !dst->obj.schema || !src->obj.schema) return false;
    for (const std::string_view name : {"StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}) {
        const repl::FastArrayState* from = observer_array(*src, name);
        repl::FastArrayState* to = observer_array(*dst, name);
        if (!from || !to) return false;
        std::vector<int32_t> gone;   // (what the shown pawn has and the player no longer has)
        for (const repl::FastArrayItem& it : to->items()) {
            if (!from->find(it.id)) gone.push_back(it.id);
        }
        for (const int32_t id : gone) {
            if (!to->remove(id)) return false;
        }
        bool added = false;
        for (const repl::FastArrayItem& it : from->items()) {
            const repl::FastArrayItem* have = to->find(it.id);
            if (!have) {
                if (!to->insert(it.id, it.values)) return false;
                added = true;
            } else if (!(have->values == it.values)) {
                if (!to->change(it.id, it.values)) return false;
            }
        }
        if (added) to->mark_array_dirty();   // an insert alone replicates nothing
    }
    return true;
}

bool PlayerBootstrap::set_observer_live_stats(repl::Actor& shown, std::span<const int64_t> records) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* dst = shown.find(setup_.guids.stats);
    const repl::Subobject* src = pawn_.find(setup_.guids.stats);
    if (!dst || !src || !dst->obj.schema || !src->obj.schema) return false;
    // PvP: contributed by MrJambix in #23.
    // the two audiences every client is sent: the int32 stats (Health and Mana) and the float ones (Corruption): both items are FStatInt32InstRep, a float's bits in its word
    for (const std::string_view name : {"StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}) {
        repl::FastArrayState* to = observer_array(*dst, name);
        const repl::FastArrayState* from = observer_array(*src, name);
        if (!to || !from) continue;
        bool added = false;
        for (const int64_t record : records) {
            const repl::FastArrayItem* mine = observer_stat_item(*from, record);
            if (!mine) continue;   // (a stat this character has no item for, or not in this array: nothing to show)
            const repl::FastArrayItem* have = observer_stat_item(*to, record);
            if (!have) {
                if (!to->insert(mine->id, mine->values)) return false;
                added = true;
            } else if (!(have->values == mine->values)) {
                if (!to->change(have->id, mine->values)) return false;
            }
        }
        if (added) to->mark_array_dirty();   // (an insert alone replicates nothing)
    }
    return true;
}

bool PlayerBootstrap::observer_live_stats_differ(const repl::Actor& shown, std::span<const int64_t> records) const {
    if (!ok_ || !second_state_added_) return false;
    const repl::Subobject* dst = shown.find(setup_.guids.stats);
    const repl::Subobject* src = pawn_.find(setup_.guids.stats);
    if (!dst || !src || !dst->obj.schema || !src->obj.schema) return false;
    // PvP: contributed by MrJambix in #23.
    for (const std::string_view name : {"StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}) {
        const repl::FastArrayState* to = observer_array(*dst, name);
        const repl::FastArrayState* from = observer_array(*src, name);
        if (!to || !from) continue;
        for (const int64_t record : records) {
            const repl::FastArrayItem* mine = observer_stat_item(*from, record);
            if (!mine) continue;
            const repl::FastArrayItem* have = observer_stat_item(*to, record);
            if (!have || !(have->values == mine->values)) return true;
        }
    }
    return false;
}

}  // namespace aoc::worldd::bootstrap

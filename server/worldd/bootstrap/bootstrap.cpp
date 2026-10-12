// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/bootstrap.cpp
//
//  See bootstrap.h.  The actors are built the way libs/aocnet/tests/
//  test_repl_actor_open.cpp builds the captured ones (the PlayerController of
//  pkt#22, the pawn of pkt#78 and #107, the PlayerState of pkt#434), from a
//  PlayerSetup instead of from captured blocks: the template that proves each
//  open and update bit for bit.  What the replicator cannot do alone is here:
//    * a property that names an object by path (the GameState's GameModeClass,
//      the PlayerController's AttachComponent) needs that object's export in the
//      chain: the blocks are collected, the export added to the block, and the
//      chain assembled (collect_*_blocks, assemble_*);
//    * ClientReceiveStoragePayload and ClientReceiveStorageEvent carry the items (plan B2): their
//      parameters are libaocnet's typed storage RPCs (storage_rpc.h), an empty slot as the original sent it.
// ============================================================================
#include "bootstrap/bootstrap.h"

#include "content/content_db.h"

#include "bootstrap/captured_gamestate.h"
#include "bootstrap/captured_level_list.h"
#include "bootstrap/captured_replies.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/schema/generated/index.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace aoc::worldd::bootstrap {

using protocol::emit::BunchWriter;
using protocol::emit::FIntrepidNetworkGUID;
using repl::ActorShadow;
using repl::Emission;
using repl::ExportChain;
using repl::Value;
namespace schema = ::aoc::protocol::schema;
namespace gen = ::aoc::protocol::schema::gen;
// (C:Y:B | CoA)

namespace {

// ---- the pieces of a value --------------------------------------------------------------------------------------------

Value object(const FIntrepidNetworkGUID& g) { return Value(g); }

/// FMath::RoundToInt: floor(x + 0.5).
int round_to_int(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

/// FRotator::CompressAxisToShort and ...ToByte: the wire integers of an angle in degrees.
uint16_t compress_short(double degrees) {
    return static_cast<uint16_t>(round_to_int(static_cast<float>(degrees) * 65536.0f / 360.0f) & 0xFFFF);
}
uint16_t compress_byte(double degrees) {
    return static_cast<uint16_t>(round_to_int(static_cast<float>(degrees) * 256.0f / 360.0f) & 0xFF);
}

/// The first 20 characters of the login name (AGameModeBase::InitNewPlayer cuts the ?Name= option to 20), whole UTF-8
/// characters.
std::string player_name_private(const std::string& option) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < option.size() && chars < 20;) {
        size_t n = 1;
        const unsigned char c = static_cast<unsigned char>(option[i]);
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        out.append(option, i, n);
        i += n;
        ++chars;
    }
    return out;
}

void write_bits_of(BunchWriter& out, const captured::CapturedBits& b) {
    const auto bytes = captured::bytes_of(b);
    out.write_bit_range(bytes.data(), 0, b.bits);
}

repl::Subobject make_component(const FIntrepidNetworkGUID& guid, const char* name, uint32_t checksum) {
    repl::Subobject s;
    s.guid = guid;
    s.name = name;
    s.checksum = checksum;
    s.obj.schema = gen::find_component_schema(name);
    return s;
}

}  // namespace

// ============================================================================================================================
//  building the actors
// ============================================================================================================================

PlayerBootstrap::PlayerBootstrap(PlayerSetup setup, GameStateMode mode) : setup_(std::move(setup)), mode_(mode) {
    ok_ = build_actors();
}

bool PlayerBootstrap::fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
    return false;
}

bool PlayerBootstrap::build_actors() {
    const PlayerSetup& s = setup_;
    const SessionGuids& g = s.guids;

    // plan B2: the items the storages hold. A slot outside its storage or an item in a slot twice is a setup this refuses, not one
    // it repairs: the payload would not carry what the character service stored.
    for (const StoredItem& it : s.items) {
        const int limit = it.storage == ItemStorage::Backpack ? storage_info(StorageKind::Backpack).slots
                          : it.storage == ItemStorage::Equipment ? storage_info(StorageKind::Equipment).slots
                                                                  : storage_info(StorageKind::QuestStorage).slots;
        if (it.slot < 0 || it.slot >= limit || !it.item.has_item) return fail("an item of the setup is in a slot outside its storage or is empty");
        if (!items_.emplace(std::make_pair(static_cast<uint8_t>(it.storage), it.slot), it.item).second) return fail("two items of the setup are in one slot");
    }
// (Cyb ~ C.O.A)

    // What no setter would catch: a number that is not a place or a time, an enum value the client does not have.
    if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z) || !std::isfinite(s.yaw_degrees)) return fail("the spawn point is not finite");
    if (!std::isfinite(s.world_time) || s.world_time < 0 || s.world_time >= 2147483648.0) {
        return fail("the world time is outside 0 .. 2^31 seconds (the PlayerState's StartTime is an int32)");
    }
    if (s.race > static_cast<uint8_t>(ClientRace::Tulnar) || s.gender > static_cast<uint8_t>(ClientGender::Female)) {
        return fail("race or gender is not a value of the client's ECharacterRace / ECharacterGender");
    }

    // Every value goes through RepState::set, which checks the path, the kind and the width and refuses what the writer
    // would refuse: a bad value fails here, with its path, not later in the middle of a join.
    auto set = [&](repl::ObjectState& o, std::string_view path, Value v) {
        if (!o.schema) return fail("no schema for " + std::string(path));
        if (!o.state.set(*o.schema, path, std::move(v))) return fail(std::string(o.schema->name) + "." + std::string(path) + " refused");
        return true;
    };
    bool ok = true;
    auto all = [&](bool b) { ok = b && ok; };

    const repl::QuantVec spawn = repl::quantize_vector(10, s.x, s.y, s.z);

    // ---- PlayerController (pkt#22) -----------------------------------------------------------------------------------
    pc_.guid = g.player_controller;
    pc_.obj.schema = gen::find_class_schema("AoCPlayerControllerBP_C");
    all(set(pc_.obj, "AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated)));
    all(set(pc_.obj, "RemoteRole", Value::u64(2)));   // AutonomousProxy: the receiver swaps Role and RemoteRole
    all(set(pc_.obj, "Role", Value::u64(4)));         // Authority
    all(set(pc_.obj, "PlayerState", object(g.player_state)));
    all(set(pc_.obj, "SpawnLocation", Value(repl::Vec3d{s.x, s.y, s.z})));
    all(set(pc_.obj, "CaravanLaunchNode", Value::i64(0)));
    all(set(pc_.obj, "SummonCooldownTimer", Value::f32(static_cast<float>(s.world_time))));
    pc_.open.archetype = pc_archetype_export();
    pc_.open.level = persistent_level_export();
    pc_.open.spawn.location = spawn;
    pc_.open.player_controller = repl::PlayerControllerOpen{0, 0};
    {
        repl::Subobject gm;   // a dynamic subobject: no schema, nothing to say, spawned from its class
        gm.guid = g.global_gm_commands;
        gm.dynamic_class = global_gm_commands_class_export();
        pc_.subobjects.push_back(std::move(gm));
    }

    // ---- GameState (pkt#28) ------------------------------------------------------------------------------------------
    gs_.guid = g.game_state;
    gs_.obj.schema = gen::find_class_schema("AoCGameStateBP_C");
    all(set(gs_.obj, "AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated)));
    all(set(gs_.obj, "RemoteRole", Value::u64(1)));   // SimulatedProxy: a replicated world actor
    all(set(gs_.obj, "Role", Value::u64(4)));
    all(set(gs_.obj, "GameModeClass", object(game_mode_class_guid())));
    all(set(gs_.obj, "bReplicatedHasBegunPlay", Value::boolean(true)));
    all(set(gs_.obj, "ReplicatedWorldTimeSecondsDouble", Value::f64(s.world_time)));
    if (mode_ != GameStateMode::Tier1) {
        const auto tier2 = game_state_tier2_values();
        if (!tier2) {
            all(fail("the captured GameState values do not read"));
        } else {
            for (const auto& [handle, value] : tier2->values()) {
                if (!gs_.obj.state.set(*gs_.obj.schema, handle, value)) all(fail("GameState handle " + std::to_string(handle) + " refused"));
            }
        }
    }
    if (mode_ == GameStateMode::Tier3) {
        // Nodes 1, stage 2: retail's login order puts the node maps in the GameState's open (capture 1198 line 47); the same arrays follow the world's levels later
        if (!repl::sync_node_maps(*gs_.obj.schema, gs_.obj.arrays, setup_.nodes)) all(fail("the node maps do not fit the GameState's fast arrays"));
    }
    gs_.open.archetype = game_state_archetype_export();
    gs_.open.level = persistent_level_export();

    // ---- the pawn, first state (pkt#78) ----------------------------------------------------------------------------------
    pawn_.guid = g.pawn;
    pawn_.obj.schema = gen::find_class_schema("PlayerPawn_C");
    all(set(pawn_.obj, "AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated)));
    {
        repl::RepMovement m;   // standing at the spawn point: location x100 as integers, the yaw as a byte, no velocity
        m.location = repl::quantize_location(pawn_.obj.opts, s.x, s.y, s.z);
        m.rotation = repl::Rot{0, compress_byte(s.yaw_degrees), 0};
        m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
        all(set(pawn_.obj, "ReplicatedMovement", Value(m)));
    }
    all(set(pawn_.obj, "RemoteRole", Value::u64(1)));   // SimulatedProxy until possession
    all(set(pawn_.obj, "Owner", object(g.player_controller)));
    all(set(pawn_.obj, "Role", Value::u64(4)));
    pawn_.open.archetype = pawn_archetype_export();
    pawn_.open.level = persistent_level_export();
    pawn_.open.spawn.location = spawn;
    pawn_.open.spawn.rotation = repl::Rot{0, compress_short(s.yaw_degrees), 0};
// C_Y_B : CoA

    {
        repl::Subobject base = make_component(g.base_character_info, "BaseCharacterInfo", kChecksumBaseCharacterInfo);
        // The CharacterGuid words are int32 on the wire.
        all(set(base.obj, "CharacterGuid.A", Value::i64(static_cast<int32_t>(s.character_guid[0]))));
        all(set(base.obj, "CharacterGuid.B", Value::i64(static_cast<int32_t>(s.character_guid[1]))));
        all(set(base.obj, "CharacterGuid.C", Value::i64(static_cast<int32_t>(s.character_guid[2]))));
        all(set(base.obj, "CharacterGuid.D", Value::i64(static_cast<int32_t>(s.character_guid[3]))));
        all(set(base.obj, "CharacterName", Value::str(s.name)));
        // The capture's character is a level-1 Kaelar male and none of the three is on its wire: the class defaults.
        if (s.race != 0) all(set(base.obj, "Race", Value::u64(s.race)));
        if (s.gender != 0) all(set(base.obj, "Gender", Value::u64(s.gender)));
        if (s.level != 1) all(set(base.obj, "Level", Value::i64(s.level)));
        all(set(base.obj, "CurrentBreathingTime", Value::f32(kBreathingTime)));
        pawn_.subobjects.push_back(std::move(base));

        repl::Subobject combat = make_component(g.combat_info, "CombatInfo", kChecksumCombatInfo);
        all(set(combat.obj, "bIsDead", Value::boolean(false)));
        all(set(combat.obj, "PrimaryClassId.Guid", Value::i64(s.class_record_guid)));
        pawn_.subobjects.push_back(std::move(combat));

        // OwnerInfo and the four storages open empty: their blocks exist to create the subobjects.  Nodes 1, stage 2 (Tier3): retail's OwnerInfo carries the node fields
        // of the player at the open (capture 1198 line 517): the account's characters with their citizen nodes, and the citizen node.  The mayor node, the reputations
        // and the warehouse tabs are the defaults until the milestones that give them values (nothing is sent for a default).
        {
            repl::Subobject owner = make_component(g.owner_info, "OwnerInfo", kChecksumOwnerInfo);
            if (mode_ == GameStateMode::Tier3) {
                if (s.citizen_node != 0) all(set(owner.obj, "CitizenNodeGuid", Value::i64(s.citizen_node)));
                if (!s.account_characters.empty()) {
                    repl::ArrayValue list;
                    for (const AccountCharacterNode& c : s.account_characters) {
                        repl::ElementState e;
                        e.values[1] = Value::str("");   // CharacterId: retail leaves it empty
                        e.values[2] = Value::str(c.guid_hex);
                        e.values[3] = Value::str(c.name);
                        e.values[4] = Value::i64(c.citizen_node);
                        list.elements.push_back(std::move(e));
                    }
                    all(set(owner.obj, "CharactersNodeData", Value(std::move(list))));
                }
            }
            pawn_.subobjects.push_back(std::move(owner));
        }
        pawn_.subobjects.push_back(make_component(g.backpack, "BackpackComponent", kChecksumItemStorage));
        pawn_.subobjects.push_back(make_component(g.equipment, "EquipmentComponent", kChecksumItemStorage));
        pawn_.subobjects.push_back(make_component(g.quest_storage, "QuestStorageComponent", kChecksumItemStorage));
        pawn_.subobjects.push_back(make_component(g.reward_storage, "RewardStorageComponent", kChecksumItemStorage));

        repl::Subobject appearance = make_component(g.appearance, "Character Appearance", kChecksumAppearance);
        all(set(appearance.obj, "SharedAppearanceInfoId.Guid", Value::i64(static_cast<int64_t>(s.shared_appearance_info_id))));
        all(set(appearance.obj, "CharacterCustomization", Value(s.customization)));
        pawn_.subobjects.push_back(std::move(appearance));
    }
    for (const repl::Subobject& sub : pawn_.subobjects) {
        if (!sub.obj.schema) all(fail(std::string("no schema for component ") + sub.name));
    }

    // ---- PlayerState (pkt#434): the connection's login, not the character ------------------------------------------------
    ps_.guid = g.player_state;
    ps_.obj.schema = gen::find_class_schema("AoCPlayerStateBP_C");
    all(set(ps_.obj, "AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated)));
    all(set(ps_.obj, "RemoteRole", Value::u64(1)));
    all(set(ps_.obj, "Owner", object(g.player_controller)));
    all(set(ps_.obj, "Role", Value::u64(4)));
    all(set(ps_.obj, "PlayerId", Value::i64(s.player_id)));
    all(set(ps_.obj, "StartTime", Value::i64(static_cast<int64_t>(s.world_time))));   // whole seconds
    {
        repl::UniqueNetId id;
        id.flags = 8;   // the NULL subsystem: the id is a plain string
        id.id = s.login.unique_id;
        all(set(ps_.obj, "UniqueId", Value(id)));
    }
    all(set(ps_.obj, "PlayerNamePrivate", Value::str(player_name_private(s.login.name_option))));
    ps_.open.archetype = player_state_archetype_export();
    ps_.open.level = persistent_level_export();
    ps_.open.spawn.location = spawn;
// C-O-A / C_Y_B

    if (!pc_.obj.schema || !gs_.obj.schema || !pawn_.obj.schema || !ps_.obj.schema) all(fail("a class schema is missing"));
    return ok;
}

PlayerBootstrap::Checkpoint PlayerBootstrap::checkpoint() {
    node_maps_before_.reset();   // (an update that was kept is final; only the one built after THIS checkpoint can be undone)
    Checkpoint c;
    c.player_controller = pc_shadow_;
    c.game_state = gs_shadow_;
    c.pawn = pawn_shadow_;
    c.player_state = ps_shadow_;
    c.second_state_sent = second_state_sent_;
    for (const auto& [id, e] : effect_actors_) c.effect_shadows[id] = e.shadow;
    // Only once the pawn's second state is added: what the join builds (the components, HeldItems from the setup) is added once and stays
    // when a chain is built again; what apply_equipment changes afterwards is what restore() must undo.
    if (second_state_added_) {
        if (const repl::Subobject* base = pawn_.find(setup_.guids.base_character_info)) c.base_arrays = base->obj.arrays;
    }
    if (xp_tracker_added_) c.leveling = leveling_;
    return c;
}

void PlayerBootstrap::restore(const Checkpoint& c) {
    pc_shadow_ = c.player_controller;
    gs_shadow_ = c.game_state;
    pawn_shadow_ = c.pawn;
    ps_shadow_ = c.player_state;
    second_state_sent_ = c.second_state_sent;
    for (auto& [id, e] : effect_actors_) {
        const auto it = c.effect_shadows.find(id);
        if (it != c.effect_shadows.end()) e.shadow = it->second;
    }
    // HeldItems as it was: apply_equipment's changes (and the array key they moved) are undone with the shadows, so the same reply built
    // again does what it did the first time
    if (c.base_arrays) {
        if (repl::Subobject* base = component(setup_.guids.base_character_info)) base->obj.arrays = *c.base_arrays;
    }
    if (c.leveling && xp_tracker_added_) {
        leveling_ = *c.leveling;
        sync_xp_tracker();
    }
    if (node_maps_before_) {   // the node maps as they were before a node_maps_update that was not sent
        gs_.obj.arrays = std::move(*node_maps_before_);
        node_maps_before_.reset();
    }
}

/// What the pawn gains at possession (pkt#107): the three components it did not have, the dynamic resource storage and
/// what the old ones now say. The order of Actor::subobjects is the order of the original's blocks: Alignment first, Ability
/// and Stats after CombatInfo, the resource storage last.
void PlayerBootstrap::add_second_state_components() {
    const PlayerSetup& s = setup_;
    const SessionGuids& g = s.guids;
    auto set = [&](repl::ObjectState& o, std::string_view path, Value v) {
        if (!o.state.set(*o.schema, path, std::move(v))) fail(std::string(o.schema->name) + "." + std::string(path) + " refused");
    };

    // the actor
    set(pawn_.obj, "RemoteRole", Value::u64(2));   // AutonomousProxy: the receiver swaps it
    set(pawn_.obj, "bHidden", Value::boolean(true));   // hidden until the phase-in
    set(pawn_.obj, "Instigator", object(g.pawn));
    set(pawn_.obj, "PlayerState", object(g.player_state));
    set(pawn_.obj, "Controller", object(g.player_controller));
    // the components it gains
    repl::Subobject alignment = make_component(g.alignment, "AlignmentComponent", kChecksumAlignment);
    {
        repl::AlignmentContainer c;
        const repl::GuidValue none{0, 0, 0, 0};
        const repl::GuidValue self{s.character_guid[0], s.character_guid[1], s.character_guid[2], s.character_guid[3]};
        c.instances = {{kAlignmentKeyA, {none}}, {kAlignmentKeyB, {none}}, {kAlignmentKeyCharacter, {self}}, {kAlignmentKeyC, {none}}};
        set(alignment.obj, "Alignments", Value(c));
    }
// -- Cyb x C-O-A --

    repl::Subobject ability = make_component(g.ability, "AbilityComponent", kChecksumAbility);
    {
        repl::ArrayValue known;
        for (const int64_t guid : s.known_abilities) {
            repl::ElementState e;
            e.values[1] = Value::i64(guid);
            known.elements.push_back(std::move(e));
        }
        set(ability.obj, "KnownAbilities", Value(std::move(known)));
        repl::ArrayValue records;
        for (const AbilityRecord& r : s.records_to_load) {
            repl::ElementState e;
            e.values[1] = Value::i64(r.guid);
            e.values[2] = Value::i64(r.type_id);
            e.values[3] = Value(repl::NameValue::hardcoded_name(0));
            records.elements.push_back(std::move(e));
        }
        set(ability.obj, "ReplicatedRecordsToLoad", Value(std::move(records)));
    }

    repl::Subobject stats = make_component(g.stats, "StatsComponent", kChecksumStats);
    {
        auto fill = [&](const char* field, const StatArray& in) {
            repl::FastArrayState* a = stats.obj.arrays.get(*stats.obj.schema, field);
            if (!a) {
                fail(std::string("StatsComponent has no fast array ") + field);
                return;
            }
            for (const StatItem& it : in.items) {
                repl::ElementState e;
                e.values[1] = Value(repl::StatInt32Inst{it.record, it.value, it.extended, it.base, it.equipment});
                if (!a->insert(it.id, std::move(e))) {
                    fail(std::string(field) + ": a stat item does not fit");
                    return;
                }
            }
            a->set_array_key(in.array_key);
        };
        fill("StatRepInt32Owner", s.stats.owner);
        fill("StatRepInt32OwnerProxy", s.stats.owner_proxy);
        fill("StatRepInt32EveryoneProxy", s.stats.everyone_proxy);
        repl::FastArrayState* f = stats.obj.arrays.get(*stats.obj.schema, "StatRepFloatArrayEveryoneProxy");
        if (!f) {
            fail("StatsComponent has no StatRepFloatArrayEveryoneProxy");
        } else {
            for (const FloatStatItem& it : s.stats.float_everyone_proxy) {
                repl::ElementState e;
                e.values[1] = Value::i64(it.record);
                e.values[2] = Value(repl::ArrayValue{});   // Values: empty
                if (!f->insert(it.id, std::move(e))) fail("StatRepFloatArrayEveryoneProxy: an item does not fit");
            }
            f->set_array_key(s.stats.float_array_key);
        }
    }

    repl::Subobject resource;   // not stably named: the client spawns it from its class
    resource.guid = g.resource_storage;
    resource.dynamic_class = resource_storage_class_export();
    resource.obj.schema = gen::find_class_schema("UResourceStorageComponent");
    if (resource.obj.schema) set(resource.obj, "ItemLocationType", Value::u64(11));   // EItemLocation::Resource0
    else fail("no schema for UResourceStorageComponent");

    pawn_.subobjects.insert(pawn_.subobjects.begin(), std::move(alignment));
    pawn_.subobjects.insert(pawn_.subobjects.begin() + 3, std::move(ability));   // after Alignment, BaseCharacterInfo, CombatInfo
    pawn_.subobjects.insert(pawn_.subobjects.begin() + 4, std::move(stats));
    pawn_.subobjects.push_back(std::move(resource));
// C-Y-B / C O A

    // what the old components gain
    if (repl::Subobject* base = component(g.base_character_info)) {
        set(base->obj, "ServerId", Value::i64(s.server_id));
        set(base->obj, "bIsPhasedOut", Value::boolean(true));
        set(base->obj, "bClientIsReadyToPhaseIn", Value::boolean(false));
        set(base->obj, "bHasBeenPossessed", Value::boolean(true));
        if (s.orgs_at_war_toast) set(base->obj, "bShouldSendOrgsAtWarWithToast", Value::boolean(true));
        if (!s.attuned_blocks.empty()) {
            repl::ArrayValue blocks;
            for (const auto& indices : s.attuned_blocks) {
                repl::ArrayValue inner;
                for (const uint32_t index : indices) {
                    repl::ElementState e;
                    e.values[1] = Value::u64(index);
                    inner.elements.push_back(std::move(e));
                }
                repl::ElementState block;
                block.values[1] = Value(std::move(inner));
                blocks.elements.push_back(std::move(block));
            }
            set(base->obj, "WorldAttunements.AttunedBlocks", Value(std::move(blocks)));
        }
        if (!s.held_items.items.empty()) {
            repl::FastArrayState* held = base->obj.arrays.get(*base->obj.schema, "HeldItems");
            if (!held) {
                fail("BaseCharacterInfo has no HeldItems");
            } else {
                for (const HeldItem& it : s.held_items.items) {
                    repl::ElementState e;
                    e.values[1] = Value::u64(it.slot);
                    e.values[2] = Value::i64(it.item_guid);
                    e.values[3] = Value::u64(it.enchantment);
                    if (!held->insert(it.id, std::move(e))) fail("HeldItems: an item does not fit");
                }
                held->set_array_key(s.held_items.array_key);
            }
        }
    }
    if (repl::Subobject* owner = component(g.owner_info)) {
        set(owner->obj, "WeaponSkillLineId.Guid", Value::i64(s.weapon_skill_line.guid));
        set(owner->obj, "WeaponSkillLineId.Name", Value(repl::NameValue::plain(s.weapon_skill_line.name)));
        // ResourceContainers has three slots; the capture names the first, the resource storage, and leaves the others null.
        repl::ArrayValue containers;
        containers.elements.resize(3);
        containers.elements[0].values[1] = object(g.resource_storage);
        set(owner->obj, "ResourceContainers", Value(std::move(containers)));
    }
    if (repl::Subobject* appearance = component(g.appearance)) {
        if (!s.appearance_ids.empty()) {
            repl::ArrayValue ids;
            for (const repl::AppearanceId& id : s.appearance_ids) {
                repl::ElementState e;
                e.values[1] = Value(id);
                ids.elements.push_back(std::move(e));
            }
            set(appearance->obj, "AppearanceIDs", Value(std::move(ids)));
        }
    }
    second_state_added_ = true;
    for (const auto& [record, value] : current_stats()) stat_layout_.insert(record);   // plan D1: what the arrays were laid out for
}

// ============================================================================================================================
//  the join sequence
// ============================================================================================================================

std::optional<Send> PlayerBootstrap::finish(Role role, bool opens, const char* what, std::optional<Emission> e) const {
    if (!e) return std::nullopt;
    Send s;
    s.role = role;
    s.opens_channel = opens;
    s.what = what;
    s.emission = std::move(*e);
    return s;
}

std::optional<Send> PlayerBootstrap::update(Role role, const char* what, repl::Actor& actor, ActorShadow& shadow) {
    bool failed = false;
    return finish(role, false, what, repl::update_actor(actor, shadow, acks_, &failed));
}

std::optional<Send> PlayerBootstrap::rpcs(Role role, const char* what, std::span<const repl::RpcRequest> calls) {
    return finish(role, false, what, repl::call_rpcs(role == Role::Pawn ? pawn_ : pc_, calls, acks_));
}

std::optional<Send> PlayerBootstrap::player_controller_open() {
    if (!ok_) return std::nullopt;
    bool failed = false;
    return finish(Role::PlayerController, true, "PlayerController open", repl::open_actor(pc_, pc_shadow_, acks_, &failed));
}

std::optional<Send> PlayerBootstrap::hud() {
    if (!ok_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientSetHUD");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "NewHUDClass", object(hud_class_guid()))) return std::nullopt;
    const ExportChain hud_class = hud_class_export();
    return finish(Role::PlayerController, false, "ClientSetHUD",
                  repl::call_rpc(pc_, nullptr, "ClientSetHUD", params, std::span<const ExportChain>(&hud_class, 1), acks_));
}

std::optional<Send> PlayerBootstrap::voice_and_level_list() {
    if (!ok_) return std::nullopt;
    const schema::ClassSchema& cls = *pc_.obj.schema;
    std::vector<repl::RpcRequest> calls(3);
    calls[0].rpc = "ClientVoiceHandshakeComplete";
    calls[1].rpc = "ClientEnableNetworkVoice";
    calls[2].rpc = "ClientUpdateMultipleLevelsStreamingStatusCompressed";
    const schema::RpcSchema* voice = cls.rpc(calls[1].rpc);
    const schema::RpcSchema* levels = cls.rpc(calls[2].rpc);
    if (!cls.rpc(calls[0].rpc) || !voice || !levels) return std::nullopt;
    if (!repl::set_rpc_param(*voice, calls[1].params, "bEnable", Value::boolean(false))) return std::nullopt;
    repl::ArrayValue bytes;   // the captured list, as one element per byte
    bytes.elements.reserve(sizeof(captured::kLevelStatuses));
    for (const uint8_t b : captured::kLevelStatuses) {
        repl::ElementState e;
        e.values[1] = Value::u64(b);
        bytes.elements.push_back(std::move(e));
    }
    if (!repl::set_rpc_param(*levels, calls[2].params, "LevelStatuses", Value(std::move(bytes)))) return std::nullopt;
    return rpcs(Role::PlayerController, "voice handshake, level streaming list", calls);
}

std::optional<Send> PlayerBootstrap::flush_bandwidth_and_time() {
    if (!ok_) return std::nullopt;
    const schema::ClassSchema& cls = *pc_.obj.schema;
    std::vector<repl::RpcRequest> calls(3);
    calls[0].rpc = "ClientFlushLevelStreaming";
    calls[1].rpc = "ClientCapBandwidth";
    calls[2].rpc = "ClientSetTime";
    const schema::RpcSchema* cap = cls.rpc(calls[1].rpc);
    const schema::RpcSchema* time = cls.rpc(calls[2].rpc);
    if (!cls.rpc(calls[0].rpc) || !cap || !time) return std::nullopt;
    const ClientTimes t = client_times(setup_.world_time);
    if (!repl::set_rpc_param(*cap, calls[1].params, "Cap", Value::i64(kBandwidthCap)) ||
        !repl::set_rpc_param(*time, calls[2].params, "NewGameTime", Value::f64(t.game_time)) ||
        !repl::set_rpc_param(*time, calls[2].params, "NewSynchronizedTime", Value::f64(t.synchronized_time))) {
        return std::nullopt;
    }
    return rpcs(Role::PlayerController, "flush, bandwidth cap, clock", calls);
}
// C Y B with C O A

std::optional<Send> PlayerBootstrap::set_time(double world_time) {
    if (!ok_) return std::nullopt;
    const schema::RpcSchema* time = pc_.obj.schema->rpc("ClientSetTime");
    if (!time) return std::nullopt;
    const ClientTimes t = client_times(world_time);
    repl::RpcParams params;
    if (!repl::set_rpc_param(*time, params, "NewGameTime", Value::f64(t.game_time)) ||
        !repl::set_rpc_param(*time, params, "NewSynchronizedTime", Value::f64(t.synchronized_time))) {
        return std::nullopt;
    }
    return finish(Role::PlayerController, false, "ClientSetTime", repl::call_rpc(pc_, nullptr, "ClientSetTime", params, {}, acks_));
}

std::optional<Emission> PlayerBootstrap::game_state_captured_open() const {
    Emission e;
    e.exports.emplace_back();
    write_bits_of(e.exports.back(), captured::kGameStateOpenExports);
    write_bits_of(e.payload, captured::kGameStateOpenContent);
    for (const captured::CapturedGuid& c : captured::kGameStateOpenExported) {
        FIntrepidNetworkGUID g;
        g.ObjectId = c.object_id;
        g.ServerId = c.server_id;
        g.Randomizer = c.randomizer;
        e.exported.push_back(g);
    }
    e.exported_by_bunch.push_back(e.exported);
    return e;
}

std::optional<Send> PlayerBootstrap::game_state_open() {
    if (!ok_) return std::nullopt;
    if (mode_ == GameStateMode::Captured) return finish(Role::GameState, true, "GameState open (captured)", game_state_captured_open());

    // GameModeClass names a class by path, which the replicator does not export on its own: add it to the actor's block.
    std::vector<repl::ContentBlock> blocks;
    repl::ShadowUndo undo;
    if (!repl::collect_open_blocks(gs_, gs_shadow_, blocks, &undo)) return std::nullopt;
    if (blocks.empty() || blocks[0].sub) {
        undo.rollback(gs_shadow_);
        return std::nullopt;
    }
    blocks[0].exports.push_back(game_mode_class_export());
    auto e = repl::assemble_open(gs_, blocks, acks_);
    if (!e) undo.rollback(gs_shadow_);
    return finish(Role::GameState, true, mode_ == GameStateMode::Tier1 ? "GameState open (tier 1)" : mode_ == GameStateMode::Tier3 ? "GameState open (tier 3)" : "GameState open (tier 2)", std::move(e));
}

std::optional<Send> PlayerBootstrap::load_tracker_before_possession() {
    if (!ok_) return std::nullopt;
    if (!pc_.obj.state.set(*pc_.obj.schema, "CharacterLoadTracker", Value(load_tracker(kLoadTrackerBeforePossession)))) return std::nullopt;
    return update(Role::PlayerController, "CharacterLoadTracker 1011...1", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::pawn_open() {
    if (!ok_) return std::nullopt;
    bool failed = false;
    return finish(Role::Pawn, true, "pawn open", repl::open_actor(pawn_, pawn_shadow_, acks_, &failed));
}

std::optional<Send> PlayerBootstrap::restart_camera_rotation() {
    if (!ok_) return std::nullopt;
    const schema::ClassSchema& cls = *pc_.obj.schema;
    std::vector<repl::RpcRequest> calls(3);
    calls[0].rpc = "ClientRestart";
    calls[1].rpc = "ClientSetCameraMode";
    calls[2].rpc = "ClientSetRotation";
    const schema::RpcSchema* restart = cls.rpc(calls[0].rpc);
    const schema::RpcSchema* camera = cls.rpc(calls[1].rpc);
    const schema::RpcSchema* rotation = cls.rpc(calls[2].rpc);
    if (!restart || !camera || !rotation) return std::nullopt;
    if (!repl::set_rpc_param(*restart, calls[0].params, "NewPawn", object(setup_.guids.pawn)) ||
        !repl::set_rpc_param(*camera, calls[1].params, "NewCamMode", Value(repl::NameValue::hardcoded_name(kCameraModeEName))) ||
        !repl::set_rpc_param(*rotation, calls[2].params, "NewRotation", Value(repl::Rot{0, compress_short(setup_.yaw_degrees), 0})) ||
        !repl::set_rpc_param(*rotation, calls[2].params, "bResetCamera", Value::boolean(true))) {
        return std::nullopt;
    }
    return rpcs(Role::PlayerController, "ClientRestart, camera mode, rotation", calls);
}

std::optional<Send> PlayerBootstrap::possession_update() {
    if (!ok_) return std::nullopt;
    const SessionGuids& g = setup_.guids;
    auto set = [&](std::string_view path, Value v) { return pc_.obj.state.set(*pc_.obj.schema, path, std::move(v)); };
    if (!set("Pawn", object(g.pawn)) || !set("ControllersOriginalPawn", object(g.pawn)) ||
        !set("CharacterLoadTracker", Value(load_tracker(kLoadTrackerAfterPossession))) ||
        !set("Name", Value::str(setup_.name)) ||
        !set("AttachmentReplication.AttachParent", object(g.pawn)) ||
        !set("AttachmentReplication.RelativeScale3D",
             Value(repl::QuantVec{repl::QuantVec::Mode::Scaled, kRelativeScale100, kRelativeScale100, kRelativeScale100})) ||
        !set("AttachmentReplication.AttachComponent", object(g.collision_cylinder))) {
        return std::nullopt;
    }
    // AttachComponent names the pawn's CollisionCylinder, a default component the client finds by name and outer: its
    // export goes with the actor's block.
    std::vector<repl::ContentBlock> blocks;
    repl::ShadowUndo undo;
    if (!repl::collect_update_blocks(pc_, pc_shadow_, blocks, &undo) || blocks.empty() || blocks[0].sub) {
        undo.rollback(pc_shadow_);
        return std::nullopt;
    }
    blocks[0].exports.push_back(repl::component_export(g.pawn, g.collision_cylinder, "CollisionCylinder", kChecksumCollisionCylinder));
    auto e = repl::assemble_blocks(pc_, blocks, acks_);
    if (!e) undo.rollback(pc_shadow_);
    return finish(Role::PlayerController, false, "PlayerController after possession", std::move(e));
}
// C-O-A / Cyb

std::optional<Send> PlayerBootstrap::pawn_second_state() {
    if (!ok_) return std::nullopt;
    if (!second_state_added_) {
        add_second_state_components();
        if (!error_.empty()) return std::nullopt;
    }
    if (second_state_sent_) return std::nullopt;   // sent once
    // The original also sent AuthServerIDReplicated's 0 and CombatInfo's bIsDead = 0 again, values the client already had
    // (the pkt#107 resend, transcript section 6.2): 33 bits that make the update equal the capture's. Forgetting them here,
    // not when the components are added, keeps a restored checkpoint's retry the same update.
    pawn_shadow_.actor.invalidate(1);
    if (const auto it = pawn_shadow_.subobjects.find(setup_.guids.combat_info.ObjectId); it != pawn_shadow_.subobjects.end()) it->second.invalidate(5);
    auto s = update(Role::Pawn, "pawn second state", pawn_, pawn_shadow_);
    if (s) second_state_sent_ = true;
    return s;
}

std::optional<Send> PlayerBootstrap::player_state_open() {
    if (!ok_) return std::nullopt;
    bool failed = false;
    return finish(Role::PlayerState, true, "PlayerState open", repl::open_actor(ps_, ps_shadow_, acks_, &failed));
}

// ---- plan D1: the XPTracker, the level and what a level grants ------------------------------------------------------------------------

namespace {

repl::DesignDataId adventuring_key() { return repl::DesignDataId::named(content::kGuidAdventuring, content::kTypeLevelingTypeDef, "Adventuring"); }
/// A skill tree's key in SkillTreePurchasesFastMap: its Guid and TypeId with the hardcoded name 0 (the retail server's replies key every
/// tree so: the class tree's id that a client request carries holds a plain string that differs between builds, the server's never).
repl::DesignDataId tree_key(int64_t guid) {
    repl::DesignDataId d;
    d.guid = guid;
    d.type_id = content::kTypeSkillTree;
    return d;
}
repl::DesignDataId rank_key(int64_t guid) {
    repl::DesignDataId d;
    d.guid = guid;
    d.type_id = content::kTypeSkillRank;
    return d;   // a rank's Name is NAME_None on the wire (leveling.h)
}

}  // namespace

void PlayerBootstrap::sync_xp_tracker() {
    for (repl::Subobject& sub : pc_.subobjects) {
        if (sub.guid == setup_.guids.xp_tracker) sub.obj.arrays = leveling_.arrays();
    }
}

bool PlayerBootstrap::set_progress(int64_t xp, int64_t debt) {
    if (!ok_ || xp < 0 || debt < 0) return false;
    if (!leveling_.set_experience(adventuring_key(), xp)) return false;
    if (!leveling_.set_experience_debt(adventuring_key(), debt)) return false;
    if (xp_tracker_added_) sync_xp_tracker();
    return true;
}

bool PlayerBootstrap::set_tree_ranks(int64_t tree, const std::vector<int64_t>& ranks) {
    if (!ok_ || tree == 0) return false;
    if (ranks.empty()) {
        leveling_.erase_purchased_ranks(tree_key(tree));   // false when there was none: nothing to send either way
    } else {
        std::vector<repl::DesignDataId> ids;
        ids.reserve(ranks.size());
        for (const int64_t g : ranks) ids.push_back(rank_key(g));
        if (!leveling_.set_purchased_ranks(tree_key(tree), ids)) return false;
    }
    if (xp_tracker_added_) sync_xp_tracker();
    return true;
}

bool PlayerBootstrap::resend_tree(int64_t tree) {
    if (!ok_ || !xp_tracker_added_ || tree == 0) return false;
    if (!leveling_.resend_purchased_ranks(tree_key(tree))) return false;
    sync_xp_tracker();
    return true;
}

std::vector<int64_t> PlayerBootstrap::tree_ranks(int64_t tree) const {
    std::vector<int64_t> out;
    if (const auto ids = leveling_.purchased_ranks(tree_key(tree))) {
        for (const repl::DesignDataId& d : *ids) out.push_back(d.guid);
    }
    return out;
}

std::optional<Send> PlayerBootstrap::xp_tracker_open() {
    if (!ok_) return std::nullopt;
    if (!xp_tracker_added_) {
        const ProgressSetup& p = setup_.progress;
        if (!set_progress(p.xp, p.debt)) return std::nullopt;
        for (const TreeSetup& t : p.trees) {
            if (!set_tree_ranks(t.tree, t.ranks)) return std::nullopt;
        }
        repl::Subobject tracker = make_component(setup_.guids.xp_tracker, "XPTracker", kChecksumXPTracker);
        if (!tracker.obj.schema) {
            fail("no schema for the XPTracker (ULevelingComponent)");
            return std::nullopt;
        }
        tracker.obj.arrays = leveling_.arrays();
        pc_.subobjects.push_back(std::move(tracker));
        xp_tracker_added_ = true;
    }
    return update(Role::PlayerController, "XPTracker", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::leveling_update() {
    if (!ok_ || !xp_tracker_added_) return std::nullopt;
    sync_xp_tracker();
    return update(Role::PlayerController, "XPTracker", pc_, pc_shadow_);
}
// == C | O | A | Cyb ==

bool PlayerBootstrap::set_level(int level) {
    if (!ok_ || level < 1 || level > 255) return false;
    repl::Subobject* base = component(setup_.guids.base_character_info);
    if (!base) return false;
    setup_.level = level;
    return base->obj.state.set(*base->obj.schema, "Level", Value::i64(level));
}

bool PlayerBootstrap::set_class(int64_t class_record_guid) {
    if (!ok_ || class_record_guid == 0) return false;
    repl::Subobject* combat = component(setup_.guids.combat_info);
    if (!combat) return false;
    setup_.class_record_guid = class_record_guid;
    return combat->obj.state.set(*combat->obj.schema, "PrimaryClassId.Guid", Value::i64(class_record_guid));
}

bool PlayerBootstrap::set_in_combat(bool in_combat) {
    if (!ok_) return false;
    repl::Subobject* combat = component(setup_.guids.combat_info);
    if (!combat || !combat->obj.schema->command("bInCombat")) return false;
    return combat->obj.state.set(*combat->obj.schema, "bInCombat", Value::boolean(in_combat));
}

bool PlayerBootstrap::set_using_ranged_weapon(bool ranged) {
    if (!ok_) return false;
    repl::Subobject* combat = component(setup_.guids.combat_info);
    if (!combat || !combat->obj.schema->command("bIsUsingRangedWeapon")) return false;
    return combat->obj.state.set(*combat->obj.schema, "bIsUsingRangedWeapon", Value::boolean(ranged));
}

bool PlayerBootstrap::set_weapon_skill_line(int64_t guid, const std::string& name) {
    if (!ok_ || !second_state_added_ || guid == 0) return false;
    repl::Subobject* owner = component(setup_.guids.owner_info);
    if (!owner) return false;
    setup_.weapon_skill_line.guid = guid;
    setup_.weapon_skill_line.name = name;
    return owner->obj.state.set(*owner->obj.schema, "WeaponSkillLineId.Guid", Value::i64(guid)) &&
           owner->obj.state.set(*owner->obj.schema, "WeaponSkillLineId.Name", Value(repl::NameValue::plain(name)));
}

bool PlayerBootstrap::mark_temp_health() {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* stats = component(setup_.guids.stats);
    if (!stats) return false;
    repl::FastArrayState* f = stats->obj.arrays.get(*stats->obj.schema, "StatRepFloatArrayEveryoneProxy");
    if (!f || f->items().empty()) return false;
    std::vector<std::pair<int32_t, repl::ElementState>> again;
    for (const repl::FastArrayItem& it : f->items()) again.emplace_back(it.id, it.values);
    for (auto& [id, values] : again) {
        if (!f->change(id, std::move(values))) return false;   // the same values: MarkItemDirty moves the keys, as retail's level-up did
    }
    return true;
}

std::optional<Send> PlayerBootstrap::pawn_state_update(const char* what) {
    if (!ok_ || !second_state_sent_) return std::nullopt;
    return update(Role::Pawn, what, pawn_, pawn_shadow_);
}

std::optional<Send> PlayerBootstrap::level_up_broadcast(int64_t category_guid, int level) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pawn_.obj.schema->rpc("BroadcastPlayLevelUpEffects");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "Category", Value::i64(category_guid)) || !repl::set_rpc_param(*rpc, params, "InLevel", Value::i64(level))) {
        return std::nullopt;
    }
    return finish(Role::Pawn, false, "BroadcastPlayLevelUpEffects", repl::call_rpc(pawn_, nullptr, "BroadcastPlayLevelUpEffects", params, {}, acks_));
}

std::optional<Send> PlayerBootstrap::skill_on_actionbar(const repl::SkillOnActionbar& u) {
    if (!ok_) return std::nullopt;
    return finish(Role::PlayerController, false, "ClientUpdateSkillOnActionbar", repl::client_update_skill_on_actionbar(pc_, u, acks_));
}

std::optional<std::vector<Send>> PlayerBootstrap::join_sequence() {
    if (!ok_) return std::nullopt;
    const Checkpoint before = checkpoint();
    const repl::ExportAcks acks_before = acks_;
    std::vector<Send> out;
    auto add = [&](std::optional<Send> s) {
        if (!s) return false;
        out.push_back(std::move(*s));
        return true;
    };
    // the items' events (plan B2) come in the retail login's order: a storage's Adds on the PlayerController, then its payload on the pawn
    // (the Backpack's Adds before the pawn's open)
    const auto add_adds = [&](ItemStorage storage) {
        auto adds = storage_adds(storage);
        if (!adds) return false;
        for (Send& s : *adds) out.push_back(std::move(s));
        return true;
    };
    const bool done =
        add(player_controller_open()) && add(hud()) && add(voice_and_level_list()) && add(flush_bandwidth_and_time()) &&
        add(game_state_open()) && add(load_tracker_before_possession()) && add_adds(ItemStorage::Backpack) && add(pawn_open()) &&
        add(storage_payload(StorageKind::Backpack, Role::Pawn)) && add_adds(ItemStorage::Equipment) &&
        add(storage_payload(StorageKind::Equipment, Role::Pawn)) && add_adds(ItemStorage::Quest) &&
        add(storage_payload(StorageKind::QuestStorage, Role::Pawn)) && add(storage_payload(StorageKind::Resource, Role::Pawn)) &&
        add(restart_camera_rotation()) && add(possession_update()) && add(pawn_second_state()) && add(player_state_open()) && add(xp_tracker_open());
    if (!done) {
        restore(before);
        acks_ = acks_before;
        return std::nullopt;
    }
    return out;
}

// ============================================================================================================================
//  replies
// ============================================================================================================================

std::optional<Send> PlayerBootstrap::phase_in() {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const SessionGuids& g = setup_.guids;
    repl::Subobject* base = component(g.base_character_info);
    if (!base) return std::nullopt;
    if (!pawn_.obj.state.set(*pawn_.obj.schema, "bHidden", Value::boolean(false)) ||
        !base->obj.state.set(*base->obj.schema, "bIsPhasedOut", Value::boolean(false)) ||
        !base->obj.state.set(*base->obj.schema, "bClientIsReadyToPhaseIn", Value::boolean(true))) {
        return std::nullopt;
    }
    return update(Role::Pawn, "phase-in", pawn_, pawn_shadow_);
}

std::optional<Send> PlayerBootstrap::combat_settings_echo(std::span<const PropertyValue> values) {
    if (!ok_) return std::nullopt;
    for (const PropertyValue& v : values) {
        if (!pc_.obj.state.set(*pc_.obj.schema, v.path, v.value)) return std::nullopt;
    }
    return update(Role::PlayerController, "combat settings echo", pc_, pc_shadow_);
}

const FIntrepidNetworkGUID& PlayerBootstrap::storage_guid(StorageKind kind) const {
    const SessionGuids& g = setup_.guids;
    switch (kind) {
        case StorageKind::Backpack: return g.backpack;
        case StorageKind::Equipment: return g.equipment;
        case StorageKind::QuestStorage: return g.quest_storage;
        case StorageKind::Resource: return g.resource_storage;
    }
    return g.backpack;
}

namespace {

/// The storage a StorageKind holds items of; the resource storage holds none.
std::optional<ItemStorage> item_storage_of(StorageKind kind) {
    switch (kind) {
        case StorageKind::Backpack: return ItemStorage::Backpack;
        case StorageKind::Equipment: return ItemStorage::Equipment;
        case StorageKind::QuestStorage: return ItemStorage::Quest;
        case StorageKind::Resource: return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

const FIntrepidNetworkGUID& PlayerBootstrap::item_storage_guid(ItemStorage storage) const {
    const SessionGuids& g = setup_.guids;
    switch (storage) {
        case ItemStorage::Backpack: return g.backpack;
        case ItemStorage::Equipment: return g.equipment;
        case ItemStorage::Quest: return g.quest_storage;
    }
    return g.backpack;
}
// [C-Y-B :: CoA]

size_t PlayerBootstrap::item_count(ItemStorage storage) const {
    size_t n = 0;
    for (const auto& [key, item] : items_) n += key.first == static_cast<uint8_t>(storage) ? 1 : 0;
    return n;
}

const repl::ItemInstance* PlayerBootstrap::item_at(ItemStorage storage, int32_t slot) const {
    const auto it = items_.find({static_cast<uint8_t>(storage), slot});
    return it == items_.end() ? nullptr : &it->second;
}

std::optional<Send> PlayerBootstrap::storage_payload(StorageKind kind, Role target) {
    if (!ok_ || (target != Role::Pawn && target != Role::PlayerController)) return std::nullopt;
    const schema::ClassSchema& cls = target == Role::Pawn ? *pawn_.obj.schema : *pc_.obj.schema;
    const schema::RpcSchema* rpc = cls.rpc("ClientReceiveStoragePayload");
    if (!rpc) return std::nullopt;
    const StorageInfo& info = storage_info(kind);

    // ClientReceiveStoragePayload(TargetStorage, NewStorageData): the plain members as RPC parameters are written (all zero), and the Storage
    // array with one element per slot -- the empty item and zeros for a slot with nothing in it (178 bits: the transcript's "692 header bits +
    // slots x 178 + 145 trailing bits"), the instance and its slot for one that has an item.
    repl::StoragePayload payload;
    payload.target = storage_guid(kind);
    payload.maximum_items = info.slots;
    payload.storage.resize(static_cast<size_t>(info.slots));
    payload.dimension = repl::Vec2d{info.dimension_x, info.dimension_y};
    if (const auto storage = item_storage_of(kind)) {
        for (auto it = items_.lower_bound({static_cast<uint8_t>(*storage), 0}); it != items_.end() && it->first.first == static_cast<uint8_t>(*storage); ++it) {
            repl::StorageSlot& slot = payload.storage[static_cast<size_t>(it->first.second)];
            slot.item = it->second;
            slot.slot = it->first.second;
        }
    }
    const auto params = repl::storage_payload_params(*rpc, payload);
    if (!params) return std::nullopt;
    BunchWriter bits;
    bool failed = false;
    if (repl::write_rpc_params(*rpc, *params, bits, {}, &failed) == 0 || failed) return std::nullopt;
    BunchWriter field;
    if (repl::write_field(cls, rpc->field_index, bits, field) == 0) return std::nullopt;

    Emission e;
    // The component is named by its GUID and the client needs its export (name, checksum, the pawn as outer) until it
    // acknowledged it. The resource storage is a dynamic subobject: its class went out with its block, and the original's
    // payloads name it with no export at all.
    std::vector<ExportChain> chains;
    if (kind != StorageKind::Resource) chains.push_back(repl::component_export(setup_.guids.pawn, storage_guid(kind), info.component_name, kChecksumItemStorage));
    repl::PackedExports packed = repl::pack_exports(chains, acks_);
    if (!packed.ok) return std::nullopt;
    e.exports = std::move(packed.bunches);
    e.exported = std::move(packed.exported);
    e.exported_by_bunch = std::move(packed.exported_by_bunch);
    repl::BlockHeader header;   // an RPC of the actor: no RepLayout block
    if (repl::write_content_block(e.payload, header, field) == 0) return std::nullopt;
    return finish(target, false, "ClientReceiveStoragePayload", std::move(e));
}

// ---- plan B2: the items' events and the pawn's visible gear --------------------------------------------------------------------

std::optional<Send> PlayerBootstrap::build_item_events(std::span<const ItemEvent> events) {
    if (!ok_ || events.empty() || events.size() > kMaxEventsPerChain) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveStorageEvent");
    if (!rpc) return std::nullopt;
    const repl::GuidValue character{setup_.character_guid[0], setup_.character_guid[1], setup_.character_guid[2], setup_.character_guid[3]};
    std::vector<repl::RpcRequest> calls;
    calls.reserve(events.size());
    for (const ItemEvent& e : events) {
        if (e.type != repl::StorageEventType::Add && e.type != repl::StorageEventType::Remove) return std::nullopt;
        if (!e.item.has_item || e.slot < 0) return std::nullopt;
        repl::ItemStorageEvent ev;
        ev.type = static_cast<uint8_t>(e.type);
        ev.item = e.item;
        if (e.instigated) ev.instigator = character;
        repl::StorageEventContainer target;
        target.object = item_storage_guid(e.storage);
        target.slots.push_back({e.slot, static_cast<int32_t>(e.item.quantity), e.item.guid});
        ev.targets.push_back(std::move(target));
        auto params = repl::storage_event_params(*rpc, ev);
        if (!params) return std::nullopt;
        repl::RpcRequest call;
        call.rpc = "ClientReceiveStorageEvent";
        call.params = std::move(*params);
        calls.push_back(std::move(call));
    }
    return rpcs(Role::PlayerController, "ClientReceiveStorageEvent", calls);
}
// ~ CoA & C Y B ~

std::optional<Send> PlayerBootstrap::loot_payload(const emit::FIntrepidNetworkGUID& loot_actor, const repl::StoragePayload& data) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveLootStoragePayload");
    if (!rpc) return std::nullopt;
    repl::LootPayload p;
    p.actor = loot_actor;
    p.data = data;
    auto params = repl::loot_payload_params(*rpc, p);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientReceiveLootStoragePayload";
    call.params = std::move(*params);
    return rpcs(Role::PlayerController, "ClientReceiveLootStoragePayload", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::loot_removed(const emit::FIntrepidNetworkGUID& container, std::span<const LootRemoval> removed) {
    if (!ok_ || !second_state_added_ || removed.empty() || removed.size() > kMaxEventsPerChain) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveStorageEvent");
    if (!rpc) return std::nullopt;
    const repl::GuidValue character{setup_.character_guid[0], setup_.character_guid[1], setup_.character_guid[2], setup_.character_guid[3]};
    std::vector<repl::RpcRequest> calls;
    for (const LootRemoval& r : removed) {
        if (!r.item.has_item || r.slot < 0) return std::nullopt;
        repl::ItemStorageEvent ev;
        ev.type = static_cast<uint8_t>(repl::StorageEventType::Remove);
        ev.item = r.item;
        ev.instigator = character;
        ev.move_source.status = repl::kStorageNoError;   // retail's removal from a container: the source is the empty container
        repl::StorageEventContainer target;
        target.object = container;
        target.slots.push_back({r.slot, static_cast<int32_t>(r.item.quantity), r.item.guid});
        ev.targets.push_back(std::move(target));
        auto params = repl::storage_event_params(*rpc, ev);
        if (!params) return std::nullopt;
        repl::RpcRequest call;
        call.rpc = "ClientReceiveStorageEvent";
        call.params = std::move(*params);
        calls.push_back(std::move(call));
    }
    return rpcs(Role::PlayerController, "ClientReceiveStorageEvent", calls);
}

bool PlayerBootstrap::set_dead(bool dead) {
    if (!ok_) return false;
    repl::Subobject* combat = component(setup_.guids.combat_info);
    if (!combat || !combat->obj.schema->command("bIsDead")) return false;
    return combat->obj.state.set(*combat->obj.schema, "bIsDead", Value::boolean(dead));
}

std::optional<Send> PlayerBootstrap::death_location(double x, double y, double z) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pawn_.obj.schema->rpc("ClientSetLastDeathLocation");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "Location", Value(repl::Vec3d{x, y, z}))) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientSetLastDeathLocation";
    call.params = std::move(params);
    return rpcs(Role::Pawn, "ClientSetLastDeathLocation", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::death_restart() {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::ClassSchema& cls = *pc_.obj.schema;
    const schema::RpcSchema* restart = cls.rpc("ClientRestart");
    const schema::RpcSchema* camera = cls.rpc("ClientSetCameraMode");
    if (!restart || !camera) return std::nullopt;
    std::vector<repl::RpcRequest> calls(4);
    for (size_t i = 0; i < 4; i += 2) {
        calls[i].rpc = "ClientRestart";
        calls[i + 1].rpc = "ClientSetCameraMode";
        if (!repl::set_rpc_param(*restart, calls[i].params, "NewPawn", object(setup_.guids.pawn)) ||
            !repl::set_rpc_param(*camera, calls[i + 1].params, "NewCamMode", Value(repl::NameValue::hardcoded_name(kCameraModeEName)))) {
            return std::nullopt;
        }
    }
    return rpcs(Role::PlayerController, "ClientRestart and camera mode (death)", calls);
}

std::optional<Send> PlayerBootstrap::respawn_choices(std::span<const int64_t> closest, int64_t home, bool previous_choice_invalid) {
    if (!ok_ || !second_state_added_ || closest.size() > 64) return std::nullopt;
    const schema::RpcSchema* rpc = pawn_.obj.schema->rpc("SendClientRespawnChoices");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    repl::ArrayValue list;
    for (const int64_t id : closest) {
        repl::ElementState e;
        e.values[1] = Value::i64(id);
        list.elements.push_back(std::move(e));
    }
    if (!repl::set_rpc_param(*rpc, params, "RespawnChoices.ClosestLegalRespawns", Value(std::move(list))) ||
        !repl::set_rpc_param(*rpc, params, "RespawnChoices.HomeRespawn", Value::i64(home)) ||
        !repl::set_rpc_param(*rpc, params, "RespawnChoices.Selection", Value::i64(0)) ||
        !repl::set_rpc_param(*rpc, params, "RespawnChoices.bPreviousChoiceInvalid", Value::boolean(previous_choice_invalid)) ||
        !repl::set_rpc_param(*rpc, params, "RespawnChoices.bHomeAttuneOnCooldown", Value::boolean(false))) {
        return std::nullopt;
    }
    repl::RpcRequest call;
    call.rpc = "SendClientRespawnChoices";
    call.params = std::move(params);
    return rpcs(Role::Pawn, "SendClientRespawnChoices", std::span<const repl::RpcRequest>(&call, 1));
}
// CoA + C Y B

std::optional<Send> PlayerBootstrap::loot_listener_removed(const emit::FIntrepidNetworkGUID& loot_actor) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientRemoveListenerFromStorage");
    if (!rpc) return std::nullopt;
    auto params = repl::remove_listener_params(*rpc, loot_actor);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientRemoveListenerFromStorage";
    call.params = std::move(*params);
    return rpcs(Role::PlayerController, "ClientRemoveListenerFromStorage", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::open_vendor(int64_t merch_set, const emit::FIntrepidNetworkGUID& vendor) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientOpenVendor");
    if (!rpc) return std::nullopt;
    auto params = repl::open_vendor_params(*rpc, merch_set, vendor);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientOpenVendor";
    call.params = std::move(*params);
    return rpcs(Role::PlayerController, "ClientOpenVendor", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::node_tax_table(const repl::NodeTaxDetails& details) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientSetNodeTaxTable");
    if (!rpc) return std::nullopt;
    auto params = repl::node_tax_table_params(*rpc, details);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientSetNodeTaxTable";
    call.params = std::move(*params);
    return rpcs(Role::PlayerController, "ClientSetNodeTaxTable", std::span<const repl::RpcRequest>(&call, 1));
}

// ---- Nodes 1, stage 2 ---------------------------------------------------------------------------------------------------------

std::optional<Send> PlayerBootstrap::node_maps_update(const repl::NodeStateMaps& maps) {
    if (!ok_ || mode_ != GameStateMode::Tier3) return std::nullopt;
    size_t changed = 0;
    repl::FastArrays before = gs_.obj.arrays;
    if (!repl::sync_node_maps(*gs_.obj.schema, gs_.obj.arrays, maps, &changed)) {
        gs_.obj.arrays = before;
        fail("the node maps do not fit the GameState's fast arrays");
        return std::nullopt;
    }
    if (changed == 0) return std::nullopt;
    auto send = update(Role::GameState, "GameState node maps", gs_, gs_shadow_);
    if (!send) gs_.obj.arrays = before;
    else node_maps_before_ = std::move(before);   // restore() undoes it if the connection does not take the chain
    return send;
}

std::optional<Send> PlayerBootstrap::node_summary(const repl::NodeSummary& summary) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveNodeSummary");
    if (!rpc) return std::nullopt;
    auto params = repl::node_summary_params(*rpc, summary);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientReceiveNodeSummary";
    call.params = std::move(*params);
    return rpcs(Role::PlayerController, "ClientReceiveNodeSummary", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::siege_summary(const repl::SiegeSummary& summary) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveSiegeSummary");
    if (!rpc) return std::nullopt;
    auto params = repl::siege_summary_params(*rpc, summary);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientReceiveSiegeSummary";
    call.params = std::move(*params);
    return rpcs(Role::PlayerController, "ClientReceiveSiegeSummary", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::siege_cooldowns() {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveAllCurrentNodeSiegeCooldowns");
    if (!rpc) return std::nullopt;
    auto params = repl::empty_siege_cooldowns_params(*rpc);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientReceiveAllCurrentNodeSiegeCooldowns";
    call.params = std::move(*params);
    return rpcs(Role::PlayerController, "ClientReceiveAllCurrentNodeSiegeCooldowns", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::item_events(std::span<const ItemEvent> events) {
    auto send = build_item_events(events);
    if (!send) return std::nullopt;
    // what the client was told is what the payloads show from now on (idempotent: a chain built again changes nothing)
    for (const ItemEvent& e : events) {
        const std::pair<uint8_t, int32_t> key{static_cast<uint8_t>(e.storage), e.slot};
        if (e.type == repl::StorageEventType::Add) items_[key] = e.item;
        else items_.erase(key);
    }
    return send;
}
// -- CoA . C_Y_B --

std::optional<std::vector<Send>> PlayerBootstrap::storage_adds(ItemStorage storage) {
    if (!ok_) return std::nullopt;
    // the Adds of what the storage holds, by slot; the chains are built without touching the copy of the storages: these are items the
    // client is about to be told of
    std::vector<ItemEvent> adds;
    for (auto it = items_.lower_bound({static_cast<uint8_t>(storage), 0}); it != items_.end() && it->first.first == static_cast<uint8_t>(storage); ++it) {
        ItemEvent e;
        e.type = repl::StorageEventType::Add;
        e.storage = storage;
        e.slot = it->first.second;
        e.item = it->second;
        adds.push_back(std::move(e));
    }
    std::vector<Send> out;
    for (size_t at = 0; at < adds.size(); at += kMaxEventsPerChain) {
        const size_t n = std::min(kMaxEventsPerChain, adds.size() - at);
        auto send = build_item_events(std::span<const ItemEvent>(adds.data() + at, n));
        if (!send) return std::nullopt;
        out.push_back(std::move(*send));
    }
    return out;
}

bool PlayerBootstrap::apply_equipment(std::span<const HeldItem> held, std::span<const repl::AppearanceId> appearance, size_t marks) {
    if (!ok_ || !second_state_added_ || marks == 0 || marks > 64) return false;
    repl::Subobject* base = component(setup_.guids.base_character_info);
    repl::Subobject* app = component(setup_.guids.appearance);
    if (!base || !app) return false;
    repl::FastArrayState* array = base->obj.arrays.get(*base->obj.schema, "HeldItems");
    if (!array) return fail("BaseCharacterInfo has no HeldItems");
    const auto state_of = [](const HeldItem& h) {
        repl::ElementState e;
        e.values[1] = Value::u64(h.slot);
        e.values[2] = Value::i64(h.item_guid);
        e.values[3] = Value::u64(h.enchantment);
        return e;
    };
    // everything is checked before anything is changed
    const bool first = array->items().empty();
    if (first) {
        std::set<int32_t> ids;
        for (const HeldItem& h : held) {
            if (h.id <= 0 || !ids.insert(h.id).second) return fail("HeldItems: an id that is not positive and unique");
        }
    } else {
        for (const HeldItem& h : held) {
            if (!array->find(h.id)) return fail("HeldItems: the item with the id " + std::to_string(h.id) + " is not in the array");
        }
    }
    repl::ArrayValue ids;
    for (const repl::AppearanceId& id : appearance) {
        repl::ElementState e;
        e.values[1] = Value(id);
        ids.elements.push_back(std::move(e));
    }
    if (!app->obj.state.set(*app->obj.schema, "AppearanceIDs", Value(std::move(ids)))) return fail("Character Appearance.AppearanceIDs refused");
    if (first) {
        for (const HeldItem& h : held) {
            if (!array->insert(h.id, state_of(h))) return fail("HeldItems: an item does not fit");
        }
        array->mark_array_dirty();   // an insert alone replicates nothing
    } else {
        for (size_t m = 0; m < marks; ++m) {
            for (const HeldItem& h : held) {
                if (!array->change(h.id, state_of(h))) return fail("HeldItems: an item does not take its change");
            }
        }
    }
    return true;
}

std::optional<Send> PlayerBootstrap::equipment_update() {
    if (!ok_ || !second_state_sent_) return std::nullopt;
    return update(Role::Pawn, "equipment", pawn_, pawn_shadow_);
}

std::optional<Send> PlayerBootstrap::verbatim_block(Role role, const char* what, const char* hex, size_t bits) {
    Emission e;
    const auto bytes = captured::bytes_of(hex);
    e.payload.write_bit_range(bytes.data(), 0, bits);
    return finish(role, false, what, std::move(e));
}

std::optional<Send> PlayerBootstrap::empty_group() {
    if (!ok_) return std::nullopt;
    return verbatim_block(Role::PlayerController, "ClientUpdateGroup (empty)", captured::kEmptyGroupBlock.hex, captured::kEmptyGroupBlock.bits);
}

std::optional<Send> PlayerBootstrap::default_action_bar() {
    if (!ok_) return std::nullopt;
    return verbatim_block(Role::PlayerController, "ClientReceiveActionbarSlotPayload", captured::kActionBarBlock.hex, captured::kActionBarBlock.bits);
}

std::optional<Send> PlayerBootstrap::action_bar(const repl::ActionbarPayload& payload) {
    if (!ok_) return std::nullopt;
    return finish(Role::PlayerController, false, "ClientReceiveActionbarSlotPayload", repl::client_receive_actionbar_slot_payload(pc_, payload, acks_));
}
// [C-Y-B + CoA]

bool PlayerBootstrap::mount_bar_calls(int64_t dash_ability, int32_t slot_index, std::vector<repl::RpcRequest>& calls) const {
    const schema::RpcSchema* clear = pc_.obj.schema->rpc("ClientClearActionbarsForInstigators");
    const schema::RpcSchema* slot = pc_.obj.schema->rpc("ClientReceiveActionbarSlotPayload");
    if (!clear || !slot) return false;
    const size_t first = calls.size();
    calls.resize(first + 2);
    calls[first].rpc = "ClientClearActionbarsForInstigators";
    calls[first + 1].rpc = "ClientReceiveActionbarSlotPayload";
    repl::ArrayValue instigators;
    for (const uint8_t who : {repl::ActionbarInstigator::Vehicle, repl::ActionbarInstigator::Mount}) {
        repl::ElementState e;
        e.values[1] = Value::u64(who);
        instigators.elements.push_back(std::move(e));
    }
    if (!repl::set_rpc_param(*clear, calls[first].params, "ActionbarInstigators", Value(std::move(instigators))) ||
        !repl::set_rpc_param(*clear, calls[first].params, "ActionBar", Value::u64(static_cast<uint64_t>(repl::ActionbarBank::Vehicle)))) {
        return false;
    }
    repl::ActionbarPayload payload;
    repl::ActionbarSlot dash;
    dash.bank_index = repl::ActionbarBank::Vehicle;
    dash.slot_index = slot_index;
    dash.use.use_type = repl::ActionbarUseType::Ability;
    dash.use.instigator = repl::ActionbarInstigator::Mount;
    dash.use.ability_id = dash_ability;
    payload.slots.push_back(dash);
    auto params = repl::actionbar_payload_params(*slot, payload);
    if (!params) return false;
    calls[first + 1].params = std::move(*params);
    return true;
}

std::optional<Send> PlayerBootstrap::mount_bar(int64_t dash_ability, int32_t slot_index) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    std::vector<repl::RpcRequest> calls;
    if (!mount_bar_calls(dash_ability, slot_index, calls)) return std::nullopt;
    return rpcs(Role::PlayerController, "the mount action bar", calls);
}

namespace {

/// The values of a CalloutQueueReplication item by handle (the schema element commands, in order: the value members, then the key FGuid).
repl::ElementState callout_state(const std::string& message, int32_t type, int32_t point_size, int64_t creation_ticks, uint64_t key) {
    repl::ElementState es;
    auto& v = es.values;
    v[1] = Value::str("");                              // Title
    v[2] = Value::str(message);                         // Message
    v[3] = Value(repl::TextValue{});                    // TextMessage
    v[4] = Value(repl::LocalizedMessage{});             // LocalizedMessage
    v[5] = Value::str("");                              // FormattedOnScreenMessage
    v[6] = Value::str("");                              // FromSourcePlayer
    v[7] = Value::i64(point_size);                      // PointSize
    v[8] = Value::u64(static_cast<uint64_t>(type));     // SetCalloutType
    v[9] = Value::u64(2);                               // ChatCategory [R: 2 in 876 and 870]
    v[10] = Value::u64(0);                              // NodeLevel
    v[11] = Value::boolean(false);                      // bIsMessageSentByGM
    v[12] = Value::str("");                             // ChatEmblems
    v[13] = Value(repl::ItemInstance{});                // ItemAssociated
    v[14] = Value::i64(0);                              // ToastRecordID
    v[15] = Value::i64(0);                              // ActionResponseID
    for (uint16_t i = 16; i <= 19; ++i) v[i] = Value::i64(0);   // ContextGuid
    v[20] = Value(repl::Vec3d{0, 0, 0});                // ContextLocation
    v[21] = Value(repl::Ticks{creation_ticks});         // CreationTime
    v[22] = Value::i64(0);                              // NodeId
    v[23] = Value::i64(0);                              // PlotId
    v[24] = Value::i64(0);                              // BuildingId
    v[25] = Value::i64(static_cast<int32_t>(key & 0xFFFFFFFFu));             // Key.A .. D: four words made of the caller key
    v[26] = Value::i64(static_cast<int32_t>((key >> 32) & 0xFFFFFFFFu));
    v[27] = Value::i64(static_cast<int32_t>(((key * 0x9E3779B97F4A7C15ull) >> 32) & 0xFFFFFFFFu));
    v[28] = Value::i64(static_cast<int32_t>(~key & 0xFFFFFFFFu));
    return es;
}

}  // namespace

std::optional<Send> PlayerBootstrap::callout_add(const std::string& message, int32_t type, int32_t point_size, int64_t creation_ticks, uint64_t key) {
    if (!ok_ || !second_state_added_ || key == 0 || callouts_removed_.count(key) != 0) return std::nullopt;
    if (callout_ids_.count(key) == 0) {
        repl::FastArrayState* array = pc_.obj.arrays.get(*pc_.obj.schema, "CalloutQueueReplication");
        if (!array) return std::nullopt;
        const auto id = array->add(callout_state(message, type, point_size, creation_ticks, key));
        if (!id) return std::nullopt;
        callout_ids_[key] = *id;
    }
    return update(Role::PlayerController, "a callout", pc_, pc_shadow_);
}
// -- CoA . C.Y.B --

std::optional<Send> PlayerBootstrap::callout_remove(uint64_t key) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const auto it = callout_ids_.find(key);
    if (it != callout_ids_.end()) {
        repl::FastArrayState* array = pc_.obj.arrays.get(*pc_.obj.schema, "CalloutQueueReplication");
        if (!array || !array->remove(it->second)) return std::nullopt;
        callout_ids_.erase(it);
        callouts_removed_.insert(key);
    } else if (callouts_removed_.count(key) == 0) {
        return std::nullopt;
    }
    return update(Role::PlayerController, "a callout is taken off", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::good_move_ack(float time_stamp) const {
    if (!ok_) return std::nullopt;
    const schema::ClassSchema& cls = *pawn_.obj.schema;
    const schema::NetField* f = cls.field("ClientMoveResponsePacked");
    if (!f) return std::nullopt;
    BunchWriter params;   // FCharacterMoveResponsePackedBits: 10 bits of header, then the move's TimeStamp
    params.write_bits(645, 10);
    params.write_bits(std::bit_cast<uint32_t>(time_stamp), 32);
    BunchWriter field;
    if (repl::write_field(cls, f->index, params, field) == 0) return std::nullopt;
    Emission e;
    e.reliable = false;   // ClientMoveResponsePacked is an unreliable RPC (function flags 0x81020c42 have no FUNC_NetReliable)
    repl::BlockHeader header;
    if (repl::write_content_block(e.payload, header, field) == 0) return std::nullopt;
    Send s;
    s.role = Role::Pawn;
    s.what = "ClientMoveResponsePacked";
    s.emission = std::move(e);
    return s;
}

// ============================================================================================================================
//  plan A7: abilities and stats after the join
// ============================================================================================================================

/// A CurrentEffects item (the 16 commands of UAoCStatsComponent field 11, in the order of the schema), as pkt#8228 and #8301 carry them: Instigator and Source are
/// the character's FGuid, SourceNode 0, no PvP force flag.
repl::ElementState effect_item_state(const std::array<uint32_t, 4>& character_guid, const EffectItem& e) {
    repl::ElementState es;
    auto& v = es.values;
    v[1] = Value::i64(e.effect_id);
    v[2] = Value::i64(e.effect_guid);
    v[3] = Value::f64(e.start_time);
    v[4] = Value::f32(e.duration);
    v[5] = Value::f32(e.tick_time);
    v[6] = Value::i64(e.stacks);
    for (uint16_t i = 0; i < 4; ++i) {
        v[static_cast<uint16_t>(7 + i)] = Value::i64(static_cast<int32_t>(character_guid[i]));    // InstigatorGuid
        v[static_cast<uint16_t>(11 + i)] = Value::i64(static_cast<int32_t>(character_guid[i]));   // SourceGuid
    }
    v[15] = Value::boolean(false);
    v[16] = Value::i64(0);
    return es;
}

/// A Cooldowns item (the 6 commands of UAoCAbilityComponent field 3; the layout of the retail Dodge replies: AbilityGuid, bCooldownTriggered, InitialCooldown, the
/// start and end times as doubles, Charges).
repl::ElementState cooldown_item_state(const CooldownItem& c) {
    repl::ElementState es;
    auto& v = es.values;
    v[1] = Value::i64(c.ability_guid);
    v[2] = Value::boolean(c.triggered);
    v[3] = Value::f32(c.initial_cooldown);
    v[4] = Value::f64(c.start_time);
    v[5] = Value::f64(c.end_time);
    v[6] = Value::i64(c.charges);
    return es;
}

namespace {

/// A GUID given as the four int32 words of an FGuid.
Value guid_word(uint32_t w) { return Value::i64(static_cast<int32_t>(w)); }

/// A CurrentEffects item (the 16 commands of UAoCStatsComponent field 11, in the order of the schema), as pkt#8228 and
/// #8301 carry them: Instigator and Source are the character's FGuid, SourceNode 0, no PvP force flag.
repl::ElementState effect_state(const PlayerSetup& s, const EffectItem& e) { return effect_item_state(s.character_guid, e); }
// [C | Y | B ~ C-O-A]

/// A ReplicatedActiveAbilities item (the 17 commands of UAoCAbilityComponent field 4), as pkt#8175 and the retail Sprint and Dodge
/// replies carry it.
repl::ElementState ability_state(const PlayerSetup& s, const ActiveAbilityItem& a) {
    repl::ElementState es;
    auto& v = es.values;
    v[1] = Value::i64(a.ability_guid);
    v[2] = Value::i64(a.server_instance_id);
    repl::ArrayValue targets;
    if (a.targets_self) {
        repl::ElementState t;
        t.values[1] = object(s.guids.collision_cylinder);
        targets.elements.push_back(std::move(t));
    }
    for (const FIntrepidNetworkGUID& g : a.target_objects) {
        repl::ElementState t;
        t.values[1] = object(g);
        targets.elements.push_back(std::move(t));
    }
    v[3] = Value(std::move(targets));
    v[4] = Value(repl::ArrayValue{});   // TargetTransforms: none
    v[5] = Value::boolean(false);       // bPvpForceFlag
    v[6] = Value::i64(0);               // ReplicatedProgressInfo.NumDisruptiveHits
    v[7] = Value::f32(0);               // .TotalDisruptionSeconds
    v[8] = Value::f32(0);               // .CastDuration
    v[9] = Value::boolean(true);        // bBranchPredictedOnAutonomous
    v[10] = Value(repl::NameValue::plain(a.montage_group));
    const std::optional<MontageRef> montage = a.montage_guid != 0 ? montage_for(a.montage_path, a.montage_guid) : montage_for(a.montage_path);
    v[11] = Value(montage ? montage->guid : FIntrepidNetworkGUID{});
    v[12] = Value::f32(1.0f);           // PlayRate
    v[13] = Value::f32(a.montage_position);
    v[14] = Value::boolean(a.may_jump_to_position);
    v[15] = Value::i64(a.animation_counter);
    v[16] = Value::u64(a.mesh_yaw);       // MeshYaw
    v[17] = object(s.guids.pawn);       // Instigator
    return es;
}

repl::ElementState cooldown_state(const CooldownItem& c) { return cooldown_item_state(c); }

/// The item of `record` in whichever stat array of `stats` holds it.
struct FoundStat {
    repl::FastArrayState* array = nullptr;
    int32_t               id = 0;
    repl::StatInt32Inst   item;
};
std::optional<FoundStat> find_stat(repl::Subobject& stats, int64_t record) {
    for (const char* field : {"StatRepInt32Owner", "StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy"}) {
        repl::FastArrayState* a = stats.obj.arrays.get(*stats.obj.schema, field);
        if (!a) continue;
        for (const repl::FastArrayItem& it : a->items()) {
            const auto found = it.values.values.find(1);
            const auto* t = found != it.values.values.end() ? found->second.get_if<repl::StatInt32Inst>() : nullptr;
            if (t && t->record == record) return FoundStat{a, it.id, *t};
        }
    }
    return std::nullopt;
}

}  // namespace

// The InstGuid of a confirmed hit: the retail ones are 6064635119377315210, ...377380756, ...378363920 (a counter of a few tens of thousands of
// every hit the server made; only that two hits never share one matters to the client).
constexpr int64_t kHitInstanceBase = 6064635119377315210ll;

bool PlayerBootstrap::apply_ability_change(const AbilityChange& c) {
    if (!ok_ || !second_state_added_) return false;
    const SessionGuids& g = setup_.guids;
    repl::Subobject* stats = component(g.stats);
    repl::Subobject* ability = component(g.ability);
    if (!stats || !ability) return false;
    repl::FastArrayState* effects = stats->obj.arrays.get(*stats->obj.schema, "CurrentEffects");
    repl::FastArrayState* abilities = ability->obj.arrays.get(*ability->obj.schema, "ReplicatedActiveAbilities");
    repl::FastArrayState* cooldowns = ability->obj.arrays.get(*ability->obj.schema, "Cooldowns");
    if (!effects || !abilities || !cooldowns) return false;

    // plan D2, stage 2: the TempHealth float array's item (the shields); a pawn that has none takes no shields
    repl::FastArrayState* shield_array = stats->obj.arrays.get(*stats->obj.schema, "StatRepFloatArrayEveryoneProxy");
    std::optional<int32_t> shield_item;
    if (c.shields) {
        if (c.shields->size() > 64) return false;
        if (shield_array) {
            for (const repl::FastArrayItem& it : shield_array->items()) {
                const auto found = it.values.values.find(1);
                const auto* record = found != it.values.values.end() ? found->second.get_if<int64_t>() : nullptr;
                if (record && *record == content::kGuidTempHealth) shield_item = it.id;
            }
        }
        // a pawn laid out without a TempHealth item (a test's) shows no shield: the rest of the change still goes
    }
    // Check everything first, in the order it is applied: a refusal leaves the state as it was.
    for (const StatChange& s : c.stats) {
        if (find_stat(*stats, s.record)) continue;
        // a stat with no item yet (plan D1, stage 2: a class change's own resource) is added to the array its audience names
        const bool add = s.add_to_replication == 1 || s.add_to_replication == 5 || s.add_to_replication == 7;
        if (!add) return false;
    }
    {
        std::set<int64_t> effect_keys, ability_keys, cooldown_keys;
        for (const auto& [k, id] : effect_ids_) effect_keys.insert(k);
        for (const auto& [k, id] : ability_ids_) ability_keys.insert(k);
        for (const auto& [k, id] : cooldown_ids_) cooldown_keys.insert(k);
        for (const int64_t k : c.effects_removed) if (effect_keys.erase(k) == 0) return false;
        for (const EffectItem& e : c.effects_added) if (!effect_keys.insert(e.effect_id).second) return false;
        for (const EffectItem& e : c.effects_changed) if (effect_keys.count(e.effect_id) == 0) return false;
        for (const int64_t k : c.abilities_removed) if (ability_keys.erase(k) == 0) return false;
        for (const ActiveAbilityItem& a : c.abilities_added) if (!ability_keys.insert(a.server_instance_id).second) return false;
        for (const ActiveAbilityItem& a : c.abilities_changed) if (ability_keys.count(a.server_instance_id) == 0) return false;
        for (const int64_t k : c.cooldowns_removed) if (cooldown_keys.erase(k) == 0) return false;
    }
// (Cyb + C.O.A)

    for (const StatChange& s : c.stats) {
        const auto found = find_stat(*stats, s.record);   // found again: two changes of one stat in a step apply one after the other
        const uint32_t bits = std::bit_cast<uint32_t>(s.value);
        const uint32_t base = std::bit_cast<uint32_t>(s.base), equipment = std::bit_cast<uint32_t>(s.equipment);
        if (!found) {
            const char* field = s.add_to_replication == 1 ? "StatRepInt32Owner" : s.add_to_replication == 5 ? "StatRepInt32OwnerProxy" : "StatRepInt32EveryoneProxy";
            repl::FastArrayState* array = stats->obj.arrays.get(*stats->obj.schema, field);
            if (!array) return fail(std::string("StatsComponent has no fast array ") + field);
            repl::ElementState es;
            es.values[1] = Value(repl::StatInt32Inst{s.record, bits, s.buckets, s.buckets ? base : 0u, s.buckets ? equipment : 0u});
            if (!array->add(std::move(es))) return fail("a stat item does not fit its array");
            stat_layout_.insert(s.record);
            continue;
        }
        const bool buckets = s.buckets && found && found->item.extended;
        if (!found || (found->item.value == bits && (!buckets || (found->item.base == base && found->item.equipment == equipment)))) {
            continue;   // nothing changes: no MarkItemDirty
        }
        repl::StatInt32Inst now = found->item;
        now.value = bits;
        if (buckets) {
            now.base = base;
            now.equipment = equipment;
        }
        repl::ElementState es;
        es.values[1] = Value(now);
        if (!found->array->change(found->id, std::move(es))) return fail("a stat item does not take its new value");
    }
    if (c.shields && shield_item) {
        repl::ElementState item;
        for (const repl::FastArrayItem& it : shield_array->items()) {
            if (it.id == *shield_item) item = it.values;
        }
        repl::ArrayValue values;
        for (const ShieldElement& e : *c.shields) {
            repl::ElementState v;
            for (uint16_t i = 0; i < 4; ++i) v.values[static_cast<uint16_t>(1 + i)] = guid_word(e.guid[i]);   // Values.Guid.A..D
            v.values[5] = Value::f32(e.base);    // Values.Base
            v.values[6] = Value::f32(e.value);   // Values.Value
            v.values[7] = Value::f32(0);         // Values.EquipmentValue
            values.elements.push_back(std::move(v));
        }
        item.values[2] = Value(std::move(values));
        if (!shield_array->change(*shield_item, std::move(item))) return fail("the TempHealth item does not take its shields");
    }
    for (const int64_t k : c.effects_removed) {
        effects->remove(effect_ids_.at(k));
        effects->mark_array_dirty();   // the original server's removals move the key by two (test_repl_fast_array)
        effect_ids_.erase(k);
    }
    for (const EffectItem& e : c.effects_added) {
        const auto id = effects->add(effect_state(setup_, e));
        if (!id) return fail("an effect item does not fit CurrentEffects");
        effect_ids_[e.effect_id] = *id;
    }
    for (const EffectItem& e : c.effects_changed) {
        if (!effects->change(effect_ids_.at(e.effect_id), effect_state(setup_, e))) return fail("an effect item does not take its change");
    }
    // E1 stage 3 fix 2: the components of other actors an item names, for the update's exports (an NPC's cylinder the client has no GUID for)
    const auto targets_of = [](const ActiveAbilityItem& a) {
        std::vector<std::pair<FIntrepidNetworkGUID, FIntrepidNetworkGUID>> out;
        for (size_t i = 0; i < a.target_objects.size() && i < a.target_actors.size(); ++i) {
            if (a.target_actors[i].is_valid() && a.target_objects[i].is_valid()) out.emplace_back(a.target_actors[i], a.target_objects[i]);
        }
        return out;
    };
    for (const int64_t k : c.abilities_removed) {
        abilities->remove(ability_ids_.at(k));
        abilities->mark_array_dirty();
        ability_ids_.erase(k);
        ability_montages_.erase(k);
        ability_targets_.erase(k);
    }
    for (const ActiveAbilityItem& a : c.abilities_added) {
        const auto id = abilities->add(ability_state(setup_, a));
        if (!id) return fail("an ability item does not fit ReplicatedActiveAbilities");
        ability_ids_[a.server_instance_id] = *id;
        ability_montages_[a.server_instance_id] = {a.montage_path, a.montage_guid};
        ability_targets_[a.server_instance_id] = targets_of(a);
    }
    for (const ActiveAbilityItem& a : c.abilities_changed) {
        if (!abilities->change(ability_ids_.at(a.server_instance_id), ability_state(setup_, a))) return fail("an ability item does not take its change");
        ability_montages_[a.server_instance_id] = {a.montage_path, a.montage_guid};
        ability_targets_[a.server_instance_id] = targets_of(a);
    }
    for (const int64_t k : c.cooldowns_removed) {
        cooldowns->remove(cooldown_ids_.at(k));
        cooldowns->mark_array_dirty();
        cooldown_ids_.erase(k);
    }
    for (const CooldownItem& cd : c.cooldowns_set) {
        const auto it = cooldown_ids_.find(cd.ability_guid);
        if (it != cooldown_ids_.end()) {
            if (!cooldowns->change(it->second, cooldown_state(cd))) return fail("a cooldown item does not take its change");
        } else {
            const auto id = cooldowns->add(cooldown_state(cd));
            if (!id) return fail("a cooldown item does not fit Cooldowns");
            cooldown_ids_[cd.ability_guid] = *id;
        }
    }
    return true;
}

bool PlayerBootstrap::set_abilities(std::span<const int64_t> known, std::span<const AbilityRecord> records) {
    if (!ok_ || !second_state_added_) return false;
    constexpr size_t kMaxLists = 4096;
    if (known.size() > kMaxLists || records.size() > kMaxLists) return false;
    repl::Subobject* ability = component(setup_.guids.ability);
    if (!ability) return false;
    repl::ArrayValue k;
    for (const int64_t guid : known) {
        repl::ElementState e;
        e.values[1] = Value::i64(guid);
        k.elements.push_back(std::move(e));
    }
    repl::ArrayValue r;
    for (const AbilityRecord& rec : records) {
        repl::ElementState e;
        e.values[1] = Value::i64(rec.guid);
        e.values[2] = Value::i64(rec.type_id);
        e.values[3] = Value(repl::NameValue::hardcoded_name(0));
        r.elements.push_back(std::move(e));
    }
    if (!ability->obj.state.set(*ability->obj.schema, "KnownAbilities", Value(std::move(k))) ||
        !ability->obj.state.set(*ability->obj.schema, "ReplicatedRecordsToLoad", Value(std::move(r)))) {
        return false;
    }
    return true;
}

bool PlayerBootstrap::has_stat_item(int64_t record) const {
    return ok_ && second_state_added_ && stat_layout_.count(record) != 0;
}
// ~ C.O.A & Cyb ~

std::map<int64_t, float> PlayerBootstrap::current_stats() {
    std::map<int64_t, float> out;
    if (!ok_ || !second_state_added_) return out;
    repl::Subobject* stats = component(setup_.guids.stats);
    if (!stats) return out;
    for (const char* field : {"StatRepInt32Owner", "StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy"}) {
        const repl::FastArrayState* a = stats->obj.arrays.get(*stats->obj.schema, field);
        if (!a) continue;
        for (const repl::FastArrayItem& it : a->items()) {
            const auto found = it.values.values.find(1);
            const auto* t = found != it.values.values.end() ? found->second.get_if<repl::StatInt32Inst>() : nullptr;
            if (t) out[t->record] = std::bit_cast<float>(t->value);
        }
    }
    return out;
}

std::optional<Send> PlayerBootstrap::ability_update(bool* failed) {
    if (failed) *failed = false;
    if (!ok_ || !second_state_sent_) return std::nullopt;
    std::vector<repl::ContentBlock> blocks;
    repl::ShadowUndo undo;
    if (!repl::collect_update_blocks(pawn_, pawn_shadow_, blocks, &undo)) {
        if (failed) *failed = true;
        fail("an ability item cannot be written (a value the codecs refuse)");
        return std::nullopt;
    }
    if (blocks.empty()) return std::nullopt;
    // A montage the update names goes with the AbilityComponent's block until the client acknowledged its export.
    for (repl::ContentBlock& b : blocks) {
        if (!b.sub || !(b.sub->guid == setup_.guids.ability)) continue;
        std::set<std::string> done;
        for (const auto& [key, montage] : ability_montages_) {
            if (montage.first.empty() || !done.insert(montage.first).second) continue;
            const auto m = montage.second != 0 ? montage_for(montage.first, montage.second) : montage_for(montage.first);
            if (m) b.exports.push_back(m->chain);
        }
        // E1 stage 3 fix 2: so does another actor's collision cylinder an item's Targets name (the client has no GUID for an NPC's component the server
        // never exported: its homing projectile had nothing to fly at) [R: retail exports the target's CollisionCylinder, flags 7 and the cylinder's
        // checksum, the NPC as its outer, in the pawn bunch that first names it; pack_exports leaves out what the client acknowledged]
        std::set<uint64_t> exported;
        for (const auto& [key, targets] : ability_targets_) {
            for (const auto& [actor, component] : targets) {
                if (exported.insert(component.ObjectId).second) b.exports.push_back(npc_component_export(actor, component));
            }
        }
    }
    auto e = repl::assemble_blocks(pawn_, blocks, acks_);
    if (!e) {
        undo.rollback(pawn_shadow_);
        if (failed) *failed = true;
        fail("the ability update does not assemble into a chain (an export that does not fit a bunch)");
    }
    return finish(Role::Pawn, false, "abilities", std::move(e));
}

std::optional<Send> PlayerBootstrap::end_ability(int32_t client_instance_id) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const repl::Subobject* ability = component(setup_.guids.ability);
    if (!ability) return std::nullopt;
    return finish(Role::Pawn, false, "ClientEndAbility", repl::client_end_ability(pawn_, *ability, client_instance_id, acks_));
}

// ---- plan D2, review: a lingering zone's actor ----------------------------------------------------------------------------------------------

bool PlayerBootstrap::lingering_add(const LingeringSpec& spec) {
    if (effect_actors_.count(spec.actor.ObjectId) != 0) return false;
    auto actor = lingering_actor_of(spec);
    if (!actor) return false;
    EffectActor e;
    e.actor = std::move(*actor);
    effect_actors_[spec.actor.ObjectId] = std::move(e);
    mesh_ = spec.pawn_mesh;
    return true;
}

repl::ExportChain PlayerBootstrap::lingering_mesh_export(const emit::FIntrepidNetworkGUID& pawn_mesh) const {
    return repl::component_export(setup_.guids.pawn, pawn_mesh, "CharacterMesh0", kChecksumPawnMesh);
}

std::optional<repl::Actor> PlayerBootstrap::lingering_actor_of(const LingeringSpec& spec) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::ClassSchema* cls = gen::find_class_schema("AAoCAbilityLingeringEffect");
    const schema::ClassSchema* component = gen::find_class_schema("UActorComponent");
    if (!cls || !component || !std::isfinite(spec.x) || !std::isfinite(spec.y) || !std::isfinite(spec.z) || !std::isfinite(spec.yaw_degrees)) return std::nullopt;
    EffectActor e;
    repl::Actor& a = e.actor;
    a.guid = spec.actor;
    a.obj.schema = cls;
    bool ok = true;
    auto set = [&](repl::ObjectState& o, std::string_view path, Value v) { ok = o.state.set(*o.schema, path, std::move(v)) && ok; };
    const auto word = [](uint32_t w) { return Value::i64(static_cast<int32_t>(w)); };   // the FGuid words are int32 on the wire
    // the values of the retail actor, in handle order (capture 44, packet 302)
    set(a.obj, "AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated));
    set(a.obj, "RemoteRole", Value::u64(1));   // SimulatedProxy
    set(a.obj, "AttachmentReplication.AttachParent", object(setup_.guids.pawn));
    set(a.obj, "AttachmentReplication.LocationOffset", Value(repl::QuantVec{repl::QuantVec::Mode::Scaled, 3, 0, 0}));
    set(a.obj, "AttachmentReplication.RelativeScale3D",
        Value(repl::QuantVec{repl::QuantVec::Mode::Scaled, kRelativeScale100, kRelativeScale100, kRelativeScale100}));
    set(a.obj, "AttachmentReplication.AttachComponent", object(spec.pawn_mesh));
    set(a.obj, "Role", Value::u64(4));   // Authority
    set(a.obj, "Instigator", object(setup_.guids.pawn));
    set(a.obj, "LingeringEffectGuid.A", word(spec.effect_guid[0]));
    set(a.obj, "LingeringEffectGuid.B", word(spec.effect_guid[1]));
    set(a.obj, "LingeringEffectGuid.C", word(spec.effect_guid[2]));
    set(a.obj, "LingeringEffectGuid.D", word(spec.effect_guid[3]));
    set(a.obj, "LingeringEffectId", Value::i64(spec.zone));
    set(a.obj, "SourceRecordGuid", Value::i64(spec.source));
    set(a.obj, "SourceRecordType", Value::i64(content::kSourceTypeAbility));
    set(a.obj, "LingeringZoneInstGuid.A", word(spec.instance_guid[0]));
    set(a.obj, "LingeringZoneInstGuid.B", word(spec.instance_guid[1]));
    set(a.obj, "LingeringZoneInstGuid.C", word(spec.instance_guid[2]));
    set(a.obj, "LingeringZoneInstGuid.D", word(spec.instance_guid[3]));
    a.open.archetype = lingering_effect_archetype_export();
    a.open.level = persistent_level_export();
    a.open.spawn.location = repl::quantize_vector(10, spec.x, spec.y, spec.z);
    a.open.spawn.rotation = repl::Rot{0, compress_short(spec.yaw_degrees), 0};
    a.open.spawn.velocity = spec.velocity;
    {
        repl::Subobject movement;
        movement.guid = spec.movement;
        movement.name = "LingeringEffectMovementComp";
        movement.checksum = kChecksumLingeringMovement;
        movement.obj.schema = component;
        set(movement.obj, "bIsActive", Value::boolean(true));
        a.subobjects.push_back(std::move(movement));
        // the two others open empty: their blocks exist to create them
        repl::Subobject ability;
        ability.guid = spec.ability;
        ability.name = "AbilityComp";
        ability.checksum = kChecksumLingeringAbility;
        a.subobjects.push_back(std::move(ability));
        repl::Subobject stats;
        stats.guid = spec.stats;
        stats.name = "StatsComp";
        stats.checksum = kChecksumLingeringStats;
        a.subobjects.push_back(std::move(stats));
    }
    if (!ok) return std::nullopt;
    return std::move(e.actor);
}

std::optional<Send> PlayerBootstrap::lingering_open(uint64_t actor_id) {
    const auto it = effect_actors_.find(actor_id);
    if (!ok_ || it == effect_actors_.end()) return std::nullopt;
    EffectActor& e = it->second;
    std::vector<repl::ContentBlock> blocks;
    repl::ShadowUndo undo;
    e.shadow.forget_sent();   // a new channel: the client has nothing of it
    if (!repl::collect_open_blocks(e.actor, e.shadow, blocks, &undo) || blocks.empty() || blocks[0].sub) {
        undo.rollback(e.shadow);
        return std::nullopt;
    }
    // AttachComponent names the pawn's CharacterMesh0, a default component the client finds by name and outer: its export goes with the actor's block
    blocks[0].exports.push_back(lingering_mesh_export(mesh_));
    auto emission = repl::assemble_open(e.actor, blocks, acks_);
    if (!emission) undo.rollback(e.shadow);
    auto send = finish(Role::Pawn, true, "lingering zone open", std::move(emission));
    if (send) send->effect_actor = actor_id;
    return send;
}

std::optional<Send> PlayerBootstrap::lingering_fade(uint64_t actor_id) {
    const auto it = effect_actors_.find(actor_id);
    if (!ok_ || it == effect_actors_.end()) return std::nullopt;
    EffectActor& e = it->second;
    if (!e.actor.obj.state.set(*e.actor.obj.schema, "bFadingOut", Value::boolean(true))) return std::nullopt;
    auto send = update(Role::Pawn, "lingering zone fades out", e.actor, e.shadow);
    if (send) send->effect_actor = actor_id;
    return send;
}

void PlayerBootstrap::lingering_remove(uint64_t actor_id) { effect_actors_.erase(actor_id); }
// (CoA + C | Y | B)

const repl::Actor* PlayerBootstrap::lingering_actor(uint64_t actor_id) const {
    const auto it = effect_actors_.find(actor_id);
    return it == effect_actors_.end() ? nullptr : &it->second.actor;
}

std::optional<Send> PlayerBootstrap::hit_confirmations(const std::vector<HitConfirm>& confirms, const repl::Vec3d& at) {
    if (!ok_ || !second_state_added_ || confirms.empty()) return std::nullopt;
    const SessionGuids& g = setup_.guids;
    repl::HitConfirmations out;
    // E1 stage 3 fix 2: the targets' collision cylinders, exported with the call unless the client acknowledged them (the client draws the impact, the hit
    // reaction and the floating number on the actor the HitComp resolves to: an unresolved one draws nothing)
    std::vector<repl::ExportChain> exports;
    std::set<uint64_t> exported;
    for (const HitConfirm& c : confirms) {
        repl::HitInstance h;
        h.inst_guid = kHitInstanceBase + static_cast<int64_t>(++hit_instances_) * 65537;
        h.record_guid = c.record_guid;
        h.source_record_type = c.source_type;
        h.source_record_guid = c.source_guid;
        h.instigator = c.instigator ? *c.instigator : g.pawn;
        if (c.source) h.source = *c.source;   // E1 stage 3 fix 3: the target a projectile landed on, for the hit that fires one from there
        else if (c.source_is_actor) h.source = c.instigator ? *c.instigator : g.pawn;
        if (!c.targets.empty()) {
            for (const HitTargetOut& o : c.targets) {
                repl::HitTarget t;
                t.hit_comp = o.component;   // the target actor [R: research/hitfx 1]
                t.impact_location = o.has_impact ? o.impact_location : at;
                if (o.has_impact) t.impact_normal = o.impact_normal;
                for (const HitStatModOut& m : o.mods) t.stat_mods.push_back({m.stat_mod_guid, m.value, m.effective});
                t.flags = o.flags;
                h.targets.push_back(std::move(t));
                if (o.cylinder.is_valid() && o.component.is_valid() && !(o.component == g.pawn) && exported.insert(o.cylinder.ObjectId).second) {
                    exports.push_back(npc_component_export(o.component, o.cylinder));
                }
            }
        } else if (!c.no_target) {
            repl::HitTarget t;
            t.hit_comp = g.pawn;
            t.impact_location = at;
            for (const HitStatModOut& m : c.mods) t.stat_mods.push_back({m.stat_mod_guid, m.value, m.effective});
            t.flags = c.flags;
            h.targets.push_back(std::move(t));
        }
        h.predicted_on_autonomous = c.predicted_autonomous;
        h.predicted_on_simulated = c.predicted_simulated;
        out.hits.push_back(std::move(h));
    }
    if (!repl::hit_confirmations_fit(out)) return std::nullopt;
    return finish(Role::PlayerController, false, "ClientHitConfirmationsReliable", repl::client_hit_confirmations(pc_, out, true, acks_, exports));
}

repl::ExportChain PlayerBootstrap::npc_component_export(const FIntrepidNetworkGUID& actor, const FIntrepidNetworkGUID& component) {
    // the same export the pawn's own cylinder gets in the pawn open (bootstrap.cpp, the possession): flags 7 (path, checksum, outer), the actor (no
    // path) as the outer [R: retail, 1,562 captures: "CollisionCylinder", checksum 0xb876932f = 3094778671, the NPC actor as the outer]
    return repl::component_export(actor, component, "CollisionCylinder", kChecksumCollisionCylinder);
}

std::optional<Send> PlayerBootstrap::correction(float time_stamp, const repl::Vec3d& location, const repl::Vec3d& velocity,
                                                uint8_t movement_mode) const {
    if (!ok_) return std::nullopt;
    const schema::RpcSchema* rpc = pawn_.obj.schema->rpc("ClientAdjustPosition");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;   // NewBase, NewBaseBoneName, bHasBase and bBaseRelativePosition stay at their defaults
    if (!repl::set_rpc_param(*rpc, params, "TimeStamp", Value::f32(time_stamp)) ||
        !repl::set_rpc_param(*rpc, params, "NewLoc", Value(location)) || !repl::set_rpc_param(*rpc, params, "NewVel", Value(velocity)) ||
        !repl::set_rpc_param(*rpc, params, "ServerMovementMode", Value::u64(movement_mode))) {
        return std::nullopt;
    }
    return finish(Role::Pawn, false, "ClientAdjustPosition", repl::call_rpc(pawn_, nullptr, "ClientAdjustPosition", params, {}, acks_));
}

}  // namespace aoc::worldd::bootstrap

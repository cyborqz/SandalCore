// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/player_setup.h
//
//  The input of worldd's native bootstrap (plan A5, spec section 4): everything
//  the sequence of one joining player is built from, typed for what the
//  replicator needs.  Plan E1 fills it from the character record and content.db,
//  plan E3 hands it to a PlayerBootstrap after NMT_Join.
//
//  Where each part comes from (docs/world-bootstrap-transcript.md section 4):
//    the character      [char]  the record: id, name, race, gender, level, appearance, position
//    the class          [C1]    content.db: the class record Guid, the stats, the abilities
//    the session        [sess]  per connection: the minted GUIDs, ServerId, world time, PlayerId
//    the login          [sess]  the NMT_Login's ?Name= option and unique id
//  and the values a level-1 character carries that nothing computes yet (the
//  weapon skill line, the attunements, the alignment keys) default to the
//  captured Bard's (spec section 5).
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/node_rpc.h"
#include "protocol/repl/value.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aoc { namespace protocol { class ServerGuidAllocator; } }

namespace aoc::worldd::bootstrap {

namespace emit = ::aoc::protocol::emit;
namespace repl = ::aoc::protocol::repl;

/// ECharacterRace as the client's exe numbers it (4 bits on the wire; transcript section 4.1, enum_values.txt).
/// The character record holds character-select ids instead (Kaelar 2, Dunir 7, Empyrean 8 are known): plan E1 maps them.
enum class ClientRace : uint8_t { Kaelar = 0, Vaelune, Vek, Renkai, Empyrean, Pyrai, Dunir, Nikua, Tulnar };

/// ECharacterGender (2 bits). The record's 1 Male / 2 Female map to these.
enum class ClientGender : uint8_t { Male = 0, Female = 1 };

// ---- stats --------------------------------------------------------------------------------------------------------

/// One item of a StatRepInt32* fast array: FStatInt32InstRep.
struct StatItem {
    int32_t  id = 0;          // FFastArraySerializer ReplicationID: non-zero and unique within its array
    int64_t  record = 0;      // the stat's record Guid
    uint32_t value = 0;       // FStatPackedInt32: the float's bits (0x41f00000 is 30.0)
    bool     extended = false;   // Base and EquipmentValue follow on the wire: the client's stat table decides
    uint32_t base = 0;
    uint32_t equipment = 0;
};

struct StatArray {
    int32_t               array_key = 0;   // ArrayReplicationKey
    std::vector<StatItem> items;
};
// [C.O.A - C.Y.B]

/// One item of StatRepFloatArrayEveryoneProxy; its Values array stays empty (the capture's one item is).
struct FloatStatItem {
    int32_t id = 0;
    int64_t record = 0;
};

/// The StatsComponent's replicated state: the Owner (the player's own client only), OwnerProxy and EveryoneProxy
/// arrays and the float array (spec section 5). The values are float bits, as sent.
struct StatsSetup {
    StatArray                  owner;
    StatArray                  owner_proxy;
    StatArray                  everyone_proxy;
    int32_t                    float_array_key = 0;
    std::vector<FloatStatItem> float_everyone_proxy;
};

// ---- abilities ----------------------------------------------------------------------------------------------------

/// One element of ReplicatedRecordsToLoad; its Name is always the hardcoded FName 0 ("None") in the capture.
struct AbilityRecord {
    int64_t guid = 0;
    int64_t type_id = 0;
};

// ---- the extras of a level-1 character ----------------------------------------------------------------------------

/// OwnerInfo.WeaponSkillLineId: a record Guid and its name. The capture's follows the equipped weapon; the default is
/// the capture's, 'Weapon_1H_Scepter'.
struct WeaponSkillLine {
    int64_t     guid = 6064632329144307267ll;
    std::string name = "Weapon_1H_Scepter";
};

/// One slot of BaseCharacterInfo.HeldItems (a fast array keyed by slot).
struct HeldItem {
    int32_t id = 0;           // ReplicationID
    uint8_t slot = 0;         // Items.Key
    int64_t item_guid = 0;    // Items.Value.ItemGuid
    uint8_t enchantment = 0;  // Items.Value.EnchantmentLevel
};

struct HeldItemsSetup {
    int32_t               array_key = 0;
    std::vector<HeldItem> items;   // empty: nothing is equipped and the array is not sent
};

// ---- the items (plan B2) ------------------------------------------------------------------------------------------------

/// The storages of the pawn that hold the character's items (the resource storage holds none in part 1).
enum class ItemStorage : uint8_t { Backpack, Equipment, Quest };

/// One item of a storage: the instance as the wire carries it, and its slot.
struct StoredItem {
    ItemStorage        storage = ItemStorage::Backpack;
    int32_t            slot = 0;
    repl::ItemInstance item;
};

// ---- the nodes (Nodes 1, stage 2) ---------------------------------------------------------------------------------

/// One character of the player's account as OwnerInfo.CharactersNodeData lists it (the node window's relationships and citizenship views): the character's
/// 32-digit id in upper case (retail's CharacterGuid; its CharacterId is empty), its name and the node the account is a citizen of (0: none).
struct AccountCharacterNode {
    std::string guid_hex;
    std::string name;
    int64_t     citizen_node = 0;
};

// ---- the login ----------------------------------------------------------------------------------------------------

/// What the PlayerState carries (transcript section 4.5): the connection's login, not the character.
struct LoginIdentity {
    /// The NMT_Login ?Name= option, whole. PlayerNamePrivate is its first 20 characters (AGameModeBase::InitNewPlayer).
    std::string name_option;
    /// The login's unique id string (FUniqueNetIdRepl, flags 8: the NULL subsystem); the capture's equals the name option.
    std::string unique_id;
};

// ---- the session --------------------------------------------------------------------------------------------------

/// The GUID of every object the bootstrap names. Dynamic objects are minted under worldd's ServerId (even ObjectIds,
/// protocol/server_guid_allocator.h); the capture's were consecutive even ids in this order, 528..566 (transcript
/// section 4.2). The GameState is the world's, not the player's: the caller says which.
struct SessionGuids {
    emit::FIntrepidNetworkGUID player_state;
    emit::FIntrepidNetworkGUID player_controller;
    emit::FIntrepidNetworkGUID global_gm_commands;     // the PlayerController's dynamic subobject
    emit::FIntrepidNetworkGUID game_state;
    emit::FIntrepidNetworkGUID pawn;
    emit::FIntrepidNetworkGUID base_character_info;    // the pawn's components, in the capture's order
    emit::FIntrepidNetworkGUID combat_info;
    emit::FIntrepidNetworkGUID owner_info;
    emit::FIntrepidNetworkGUID backpack;
    emit::FIntrepidNetworkGUID equipment;
    emit::FIntrepidNetworkGUID quest_storage;
    emit::FIntrepidNetworkGUID reward_storage;
    emit::FIntrepidNetworkGUID appearance;
    emit::FIntrepidNetworkGUID resource_storage;       // dynamic: created at the pawn's second state
    emit::FIntrepidNetworkGUID collision_cylinder;     // the pawn's default component the PC attaches to
    emit::FIntrepidNetworkGUID alignment;
    emit::FIntrepidNetworkGUID ability;
    emit::FIntrepidNetworkGUID stats;
    /// Plan D1: the PlayerController's XPTracker (ULevelingComponent), minted after the pawn's components (the capture's was
    /// the last id of the player's, 578, after everything else).
    emit::FIntrepidNetworkGUID xp_tracker;
};

// ---- the progress (plan D1) --------------------------------------------------------------------------------------------------

/// The ranks the XPTracker lists for one skill tree: every rank the character has in it (the level's auto-granted ones, then the bought
/// ones), as SkillTreePurchasesFastMap's item shows them.
struct TreeSetup {
    int64_t              tree = 0;       // the SkillTree's Guid
    std::vector<int64_t> ranks;          // SkillRank guids, in the order of the list
};

/// What the XPTracker says at the join: the Adventuring XP total and debt, and the trees the character has ranks in (SkillTreePurchasesFastMap:
/// the class tree, the professions', the Universal tree, the weapon trees with a bought rank; a tree with no rank is not sent).
struct ProgressSetup {
    int64_t                xp = 0;
    int64_t                debt = 0;
    std::vector<TreeSetup> trees;
};

/// Mints every GUID of a joining player but the GameState's, in the capture's order (PlayerState, PlayerController,
/// GlobalGMCommands, the pawn, its components).
SessionGuids mint_session_guids(protocol::ServerGuidAllocator& allocator, const emit::FIntrepidNetworkGUID& game_state);

// ---- everything -----------------------------------------------------------------------------------------------------

/// SharedAppearanceInfoId.Guid of the captured character: a Guid in the shared-appearance table whose source is unknown
/// (transcript section 4.6), so every character gets this one.
inline constexpr uint64_t kDefaultSharedAppearanceInfoId = 6064629977195085824ull;
// (C_Y_B) (CoA)

struct PlayerSetup {
    // ---- the character [char] ------------------------------------------------------------------------------------
    /// BaseCharacterInfo.CharacterGuid A..D: the record's 32 hex digits as four big-endian words.
    std::array<uint32_t, 4> character_guid{};
    std::string             name;
    uint8_t                 race = 0;       // ClientRace
    uint8_t                 gender = 0;     // ClientGender
    int32_t                 level = 1;
    /// CombatInfo.PrimaryClassId.Guid: the class's record Guid ([C1]: the Bard is 108640158570105).
    int64_t                 class_record_guid = 0;
    repl::CharacterCustomization customization;
    uint64_t                shared_appearance_info_id = kDefaultSharedAppearanceInfoId;
    /// The spawn point, in cm, and the yaw in degrees.
    double                  x = 0, y = 0, z = 0;
    double                  yaw_degrees = 0;

    // ---- stats and abilities [C1] -------------------------------------------------------------------------------
    StatsSetup                 stats;
    std::vector<int64_t>       known_abilities;      // AbilityComponent.KnownAbilities
    std::vector<AbilityRecord> records_to_load;      // AbilityComponent.ReplicatedRecordsToLoad

    // ---- what a level-1 character carries; the captured Bard's by default -------------------------------------------
    WeaponSkillLine                  weapon_skill_line;
    /// BaseCharacterInfo.WorldAttunements.AttunedBlocks: a block of attuned indices each; the capture has one block with
    /// index 1.
    std::vector<std::vector<uint32_t>> attuned_blocks = {{1}};
    HeldItemsSetup                   held_items;
    /// Character Appearance.AppearanceIDs: the equipped appearance items. Empty for an unequipped character.
    std::vector<repl::AppearanceId>  appearance_ids;
    /// Plan B2: the character's items in its three storages, in any order: what the join's storage payloads and ClientReceiveStorageEvent
    /// calls carry. Empty for a character that has none (the storages are sent empty, as before plan B2).
    std::vector<StoredItem>          items;
    /// Plan D1: the XPTracker's state at the join.
    ProgressSetup                    progress;
    /// BaseCharacterInfo.bShouldSendOrgsAtWarWithToast: the client shows "Your Settlement is at war!" after login.
    /// The captured character's settlement was at war (pkt#107 sends 1); a character with no settlement at war leaves
    /// it at its default, false, and it is not sent (in-game check 2026-10-01: with 1 the toast showed for cyb).
    bool                             orgs_at_war_toast = false;

    // ---- the nodes (Nodes 1, stage 2): used by a GameStateMode::Tier3 join ---------------------------------------------
    /// The GameState's node maps at the join (the registry's levels now; world/node_wire.h makes them).
    repl::NodeStateMaps                nodes;
    /// OwnerInfo.CharactersNodeData: one entry per character of the account (just this one when the account's list is not known).
    std::vector<AccountCharacterNode>  account_characters;
    /// OwnerInfo.CitizenNodeGuid (the account's node; 0 until the citizenship milestone).
    int64_t                            citizen_node = 0;

    // ---- the login [sess] ----------------------------------------------------------------------------------------
    LoginIdentity login;

    // ---- the session [sess] --------------------------------------------------------------------------------------
    uint32_t     server_id = 0;       // worldd's ServerId: BaseCharacterInfo.ServerId and the GUIDs' server
    /// The server's clock in seconds (its uptime in the capture): the GameState's world time, the PlayerController's
    /// SummonCooldownTimer, the PlayerState's StartTime (whole seconds) and what ClientSetTime derives from.
    double       world_time = 0;
    int32_t      player_id = 0;       // PlayerState.PlayerId
    SessionGuids guids;
};

}  // namespace aoc::worldd::bootstrap

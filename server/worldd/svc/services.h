// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/services.h
//
//  What worldd asks the platform services (architecture spec §2): redeem a
//  join ticket (gateway), load a character (character), register and send
//  heartbeats (mesh).  Interfaces, so the login flow and the endpoint can be
//  tested with fakes; svc/grpc_services.h implements them over gRPC.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace worldd { namespace svc {

/// How a call ended, from worldd's point of view.
enum class CallStatus {
    Ok,
    Refused,      // the service said no (PermissionDenied, InvalidArgument)
    NotFound,
    Unavailable,  // unreachable, timed out, or shutting down: worth retrying
    Failed,       // anything else
};

struct RedeemResult {
    CallStatus status = CallStatus::Failed;
    int64_t account_id = 0;
    std::string character_id;
};

struct Position {
    double x = 0, y = 0, z = 0;
    float yaw = 0;
};

/// The storages a character's items are in (aoc.character.v1.ItemContainer, plan B2): the Backpack (70 slots), the Equipment
/// (83: the slot is the client's EEquipSlot) and the Quest storage (100).
enum class ItemContainer : uint8_t { Backpack = 0, Equipment = 1, Quest = 2 };

/// One item instance of a character (aoc.character.v1.Item).
struct InventoryItem {
    ItemContainer container = ItemContainer::Backpack;
    int32_t       slot = 0;
    int64_t       item_record_id = 0;        // the design database's ItemRecord Guid
    std::string   instance_guid;             // the instance's FGuid: 32 lowercase hex digits (the four int32 words in order)
    int32_t       quantity = 1;
    int32_t       quality = 1;
    float         durability = 0;
    int64_t       created_at_ms = 0;         // Unix milliseconds
    /// The state a mount item persists (the mounts milestone; character service migration 0007; retail's login shows it on every mount item as the two
    /// persisted stats and the LockExpiration of its FItemInstance): the mount's Health and Mana as they were when it was last desummoned or died, and the
    /// Unix second until which the item cannot be summoned (0 or a past time: it is not locked).  Absent and 0 on every other item.
    std::optional<float> persisted_health;
    std::optional<float> persisted_mana;
    int64_t       lock_expiration = 0;
    bool operator==(const InventoryItem& o) const {
        const auto same = [](const std::optional<float>& a, const std::optional<float>& b) {
            return a.has_value() == b.has_value() && (!a || std::memcmp(&*a, &*b, sizeof(float)) == 0);
        };
        return container == o.container && slot == o.slot && item_record_id == o.item_record_id && instance_guid == o.instance_guid &&
               quantity == o.quantity && quality == o.quality && std::memcmp(&durability, &o.durability, sizeof durability) == 0 &&
               created_at_ms == o.created_at_ms && same(persisted_health, o.persisted_health) && same(persisted_mana, o.persisted_mana) &&
               lock_expiration == o.lock_expiration;
    }
};

/// Plan D1: the XP a character has in one leveling type (aoc.character.v1.Experience): the design database's LevelingTypeDef
/// Guid (Adventuring 108651085518458), the total as the client's XPTracker shows it and the experience debt.
struct Experience {
    int64_t leveling_type_guid = 0;
    int64_t xp = 0;
    int64_t debt = 0;
    bool operator==(const Experience& o) const { return leveling_type_guid == o.leveling_type_guid && xp == o.xp && debt == o.debt; }
};
// C:Y:B : C.O.A

/// Plan D1, stage 2: the ranks a character bought in one skill tree (aoc.character.v1.TreeRanks): the tree's Guid and the SkillRank Guids in the
/// order they were bought. The ranks a level grants by itself are not stored.
struct TreeRanks {
    int64_t              tree_guid = 0;
    std::vector<int64_t> rank_guids;
    bool operator==(const TreeRanks&) const = default;
};

/// Plan D1, stage 2: one slot of the action bar (aoc.character.v1.ActionbarSlot): FActionbarSlotUseData with its bank and slot.
struct ActionbarSlot {
    int32_t     bank = 0;
    int32_t     slot = 0;
    int32_t     use_type = 0;
    int32_t     instigator = 0;
    int64_t     ability_id = 0;
    int64_t     skill_rank_id = 0;
    int64_t     pet_command_id = 0;
    int64_t     item_record_id = 0;
    std::string item_guid;   // 32 lowercase hex digits, or empty
    int32_t     item_rarity = 0;
    int32_t     item_quality = 0;
    int32_t     combo_button = 0;
    bool operator==(const ActionbarSlot&) const = default;
};

/// A character record (aoc.character.v1.Character). race_id, gender_id and class_id are the client's own enums
/// (plan B1): ECharacterRace 0 Kaelar ... 7 Nikua (8 Tulnar is never created), ECharacterGender 0 Male / 1 Female,
/// ECharacterClass 1 Ranger, 2 Bard, 3 Cleric, 4 Fighter, 5 Rogue, 6 Summoner, 7 Tank, 8 Mage (0: none, a record whose
/// class the character service's migration could not derive).
/// One character of an account as the node window lists it.
struct AccountCharacter {
    std::string id;     // 32 hex digits
    std::string name;
};

struct Character {
    std::string id;
    int64_t account_id = 0;
    std::string name;
    int32_t race_id = 0;
    int32_t gender_id = 0;
    int32_t class_id = 0;
    int32_t archetype_id = 0;
    std::string class_name;
    std::string appearance_json;
    int32_t level = 0;
    bool has_position = false;
    Position position;
    std::string last_region;
    /// Plan B2: the character's items (GetCharacter with include_items), the Backpack's by slot, then the Equipment's, then the
    /// Quest storage's. A character that was never given a kit has none.
    std::vector<InventoryItem> items;
    /// Plan D1: the character's XP per leveling type (with the items). A character that never earned any has none.
    std::vector<Experience> experience;
    /// Plan D1, stage 2: the ranks the character bought per tree, and its action bar (with the items). A character that never bought a rank
    /// or arranged its bar has none.
    std::vector<TreeRanks>     skill_ranks;
    std::vector<ActionbarSlot> action_bar;
    /// Nodes 1, stage 2: the characters of the same account (id, name), this one included, for OwnerInfo.CharactersNodeData.  Empty when the list is not known
    /// (a service that does not answer ListCharacters, a fake): the join lists this character alone.
    std::vector<AccountCharacter> account_characters;
};

/// Plan D1: what SaveProgress writes: a zero level or class leaves that value; every row replaces the character's row of its type.
struct Progress {
    int32_t                 level = 0;
    int32_t                 class_id = 0;
    std::vector<Experience> experience;
    /// Plan D1, stage 2: with replace_skill_ranks the character's bought ranks become exactly `skill_ranks` (a tree not listed has none left); with
    /// replace_action_bar its bar becomes exactly `action_bar`. Without them they are left alone.
    bool                       replace_skill_ranks = false;
    std::vector<TreeRanks>     skill_ranks;
    bool                       replace_action_bar = false;
    std::vector<ActionbarSlot> action_bar;
    bool operator==(const Progress& o) const {
        if (level != o.level || class_id != o.class_id || experience.size() != o.experience.size()) return false;
        for (size_t i = 0; i < experience.size(); ++i) {
            if (!(experience[i] == o.experience[i])) return false;
        }
        return replace_skill_ranks == o.replace_skill_ranks && skill_ranks == o.skill_ranks && replace_action_bar == o.replace_action_bar &&
               action_bar == o.action_bar;
    }
};
// [C:Y:B :: C O A]

struct CharacterResult {
    CallStatus status = CallStatus::Failed;
    Character character;
};

struct RegisterResult {
    CallStatus status = CallStatus::Failed;
    uint32_t server_id = 0;
    int64_t heartbeat_interval_ms = 0;
};

class GatewayApi {
public:
    virtual ~GatewayApi() = default;
    virtual RedeemResult redeem_ticket(const std::string& ticket, const std::string& character_id,
                                       uint32_t server_id) = 0;
};

class CharacterApi {
public:
    virtual ~CharacterApi() = default;
    virtual CharacterResult get_character(const std::string& id) = 0;
    /// SaveCharacterState with a position: the character's last good position after play (plan A6). The service
    /// leaves last_region and level unchanged (they are not sent). Refused: a position the store rejects (not
    /// finite); NotFound: no such character.
    virtual CallStatus save_position(const std::string& id, const Position& position) = 0;
    /// Plan B2: SaveInventory, which replaces all of the character's items with `items` (the latest save wins; worldd is the
    /// only writer while the character plays). Refused: items the service's rules refuse (a slot outside its storage, two items in
    /// a slot, a quantity out of range ...) or none at all; NotFound: no such character. A fake that does not care leaves the default.
    virtual CallStatus save_inventory(const std::string& id, const std::vector<InventoryItem>& items) {
        (void)id;
        (void)items;
        return CallStatus::Failed;
    }
    /// Plan D1: SaveProgress: the level, the class and the XP rows, in one transaction (the latest save wins). Refused: a level
    /// outside 1-25, a class outside 1-8, a leveling type of 0, a negative XP or debt; NotFound: no such character.
    virtual CallStatus save_progress(const std::string& id, const Progress& progress) {
        (void)id;
        (void)progress;
        return CallStatus::Failed;
    }
};

class MeshApi {
public:
    virtual ~MeshApi() = default;
    virtual RegisterResult register_server(const std::string& address,
                                           const std::vector<std::string>& regions) = 0;
    virtual CallStatus heartbeat(uint32_t server_id) = 0;
};

inline const char* call_status_name(CallStatus s) {
    switch (s) {
    case CallStatus::Ok: return "ok";
    case CallStatus::Refused: return "refused";
    case CallStatus::NotFound: return "not found";
    case CallStatus::Unavailable: return "unavailable";
    case CallStatus::Failed: return "failed";
    }
    return "?";
}

}}} // namespace aoc::worldd::svc

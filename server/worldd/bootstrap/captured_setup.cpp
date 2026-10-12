// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/captured_setup.cpp
// ============================================================================
#include "bootstrap/captured_setup.h"

#include "bootstrap/captured_abilities.h"
#include "bootstrap/captured_character.h"
#include "bootstrap/captured_stats.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"

#include <iterator>

namespace aoc::worldd::bootstrap {

namespace {

StatArray stat_array(int32_t key, const captured::StatEntry* first, const captured::StatEntry* last) {
    StatArray a;
    a.array_key = key;
    for (const captured::StatEntry* e = first; e != last; ++e) {
        StatItem it;
        it.id = e->id;
        it.record = e->record;
        it.value = e->value;
        it.extended = e->extended;
        it.base = e->base;
        it.equipment = e->equipment;
        a.items.push_back(it);
    }
    return a;
}

emit::FIntrepidNetworkGUID session_guid(uint64_t id) {
    emit::FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = kCapturedServerId;
    g.Randomizer = kCapturedRandomizer;
    return g;
}

}  // namespace

StatsSetup captured_bard_level1_stats() {
    using namespace captured;
    StatsSetup s;
    s.owner = stat_array(kStatOwnerKey, std::begin(kStatOwner), std::end(kStatOwner));
    s.owner_proxy = stat_array(kStatOwnerProxyKey, std::begin(kStatOwnerProxy), std::end(kStatOwnerProxy));
    s.everyone_proxy = stat_array(kStatEveryoneProxyKey, std::begin(kStatEveryoneProxy), std::end(kStatEveryoneProxy));
    s.float_array_key = kStatFloatEveryoneProxyKey;
    for (const FloatStatEntry& f : kStatFloatEveryoneProxy) s.float_everyone_proxy.push_back({f.id, f.record});
    return s;
}
// == Cyb | C-O-A ==

std::vector<int64_t> captured_bard_known_abilities() {
    return std::vector<int64_t>(std::begin(captured::kKnownAbilities), std::end(captured::kKnownAbilities));
}

std::vector<AbilityRecord> captured_bard_records_to_load() {
    std::vector<AbilityRecord> out;
    for (const captured::RecordToLoad& r : captured::kRecordsToLoad) out.push_back({r.guid, r.type_id});
    return out;
}

std::optional<PlayerSetup> captured_character_setup() {
    PlayerSetup s;
    // BaseCharacterInfo.CharacterGuid A..D (pkt#78): the character id D8B2FE19 95FB4DF9 9B1E3958 6C2B2BD4
    s.character_guid = {3635609113u, 2516274681u, 2602449240u, 1814768596u};
    s.name = "RandomChar";
    s.race = static_cast<uint8_t>(ClientRace::Kaelar);
    s.gender = static_cast<uint8_t>(ClientGender::Male);
    s.level = 1;
    s.class_record_guid = 108640158570105ll;   // the Bard (CombatInfo.PrimaryClassId.Guid, pkt#78)
    // the spawn (pkt#22 SpawnLocation) and the yaw whose compressed short is 16550 (pkt#78 rotation)
    s.x = -777664.5;
    s.y = 616578.6875;
    s.z = 15952.0498046875;
    s.yaw_degrees = 16550.0 * 360.0 / 65536.0;

    // the appearance block: SharedAppearanceInfoId.Guid and CharacterCustomization (pkt#78), read through the codec
    const auto* appearance = protocol::schema::gen::find_component_schema("Character Appearance");
    if (!appearance) return std::nullopt;
    {
        const captured::CapturedBits& b = captured::kAppearanceOpenBlock;
        const auto bytes = captured::bytes_of(b);
        size_t pos = 0;
        const auto st = repl::read_rep_block(*appearance, bytes.data(), b.bits, pos);
        if (!st || pos != b.bits) return std::nullopt;
        const auto* shared = st->get(*appearance, "SharedAppearanceInfoId.Guid");
        const auto* custom = st->get(*appearance, "CharacterCustomization");
        const auto* shared_id = shared ? shared->get_if<int64_t>() : nullptr;
        const auto* value = custom ? custom->get_if<repl::CharacterCustomization>() : nullptr;
        if (!shared_id || !value) return std::nullopt;
        s.shared_appearance_info_id = static_cast<uint64_t>(*shared_id);
        s.customization = *value;
    }

    s.stats = captured_bard_level1_stats();
    s.known_abilities = captured_bard_known_abilities();
    s.records_to_load = captured_bard_records_to_load();

    // the captured character's settlement was at war: BaseCharacterInfo.bShouldSendOrgsAtWarWithToast = 1 (pkt#107)
    s.orgs_at_war_toast = true;

    // the equipped items and what they show (pkt#107); the weapon skill line and the attunement are PlayerSetup's defaults
    s.held_items.array_key = captured::kHeldItemsKey;
    for (const captured::HeldItemEntry& h : captured::kHeldItems) {
        s.held_items.items.push_back({h.id, static_cast<uint8_t>(h.key), h.item_guid, static_cast<uint8_t>(h.enchantment)});
    }
    for (const captured::AppearanceIdEntry& a : captured::kAppearanceIds) {
        repl::AppearanceId id;
        id.record_guid = static_cast<uint64_t>(a.record_guid);
        id.type_id = static_cast<uint64_t>(a.type_id);
        id.enchantment_level = a.enchantment;
        id.item_equip_slot = a.equip_slot;
        s.appearance_ids.push_back(id);
    }

    // the login (pkt#434): the e2e client's NMT_Login ?Name= option and unique id are one string
    s.login.name_option = "DESKTOP-EXAMPLE1-US-45739BC044607BD9A50196AF41F22492";
    s.login.unique_id = s.login.name_option;

    // the session (transcript section 4.2)
    s.server_id = kCapturedServerId;
    s.world_time = 1593462.375;   // pkt#22 SummonCooldownTimer; also StartTime 1593462 (pkt#434)
    s.player_id = 1874;
    SessionGuids& g = s.guids;
    g.player_state = session_guid(10341528);
    g.player_controller = session_guid(10341530);
    g.global_gm_commands = session_guid(10341532);
    g.game_state = session_guid(4608);
    g.pawn = session_guid(10341538);
    g.base_character_info = session_guid(10341540);
    g.combat_info = session_guid(10341542);
    g.owner_info = session_guid(10341544);
    g.backpack = session_guid(10341546);
    g.equipment = session_guid(10341548);
    g.quest_storage = session_guid(10341550);
    g.reward_storage = session_guid(10341552);
    g.appearance = session_guid(10341554);
    g.resource_storage = session_guid(10341556);
    g.collision_cylinder = session_guid(10341558);
    g.alignment = session_guid(10341560);
    g.ability = session_guid(10341564);
    g.stats = session_guid(10341566);
    g.xp_tracker = session_guid(10341578);   // plan D1: the capture's XPTracker (transcript section 4.2)
    return s;
}

}  // namespace aoc::worldd::bootstrap

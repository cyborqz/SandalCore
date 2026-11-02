// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_pets.cpp: the Summoner's pet.  The summon's effect on the pawn says a pet is wanted; the session brings its actor into the world, keeps it near the player, answers the pet commands
//  and its stances, fills the pet bar and runs the auto mode.  Its fights are the world's (combat/combat_pets.cpp).
//
//  Modelled, no recording opens a pet: the owner's name on its alignments, both ways the client can ask for a pet's ability, the Stance Swap's toggle, a pet's Cooldowns, the slots the pet bar fills,
//  the leash's 40 m and where the bolt puts the pet.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/constants.h"
#include "combat/combat_world.h"
#include "session/player_combat_side.h"
#include "session/player_session_util.h"
#include "session/summoner_pets.h"
#include "spawns/captured_spawns.h"
#include "spawns/generic_npc.h"

#include <fmt/format.h>

#include <cmath>
#include <memory>

#include <spdlog/spdlog.h>

namespace aoc::worldd {

void PlayerSession::clear_pet() {
    if (pet_actor_ && npc_world_ && npc_world_->combat()) npc_world_->combat()->pet_gone(pet_actor_);
    pet_held_target_ = 0;
    pet_incap_target_ = 0;
    pet_stance_ = pets::Stance::Aggressive;
    pet_cooldowns_.clear();
    pet_cooldown_items_.clear();
    pet_chasing_owner_ = false;
    if (hunter_stance_set_ && pipeline_) {   // the owner's reference to the stance goes with the Hunter
        pipeline_->remove_effect(pets::kHunterRefMelee, ability_seconds(pet_clock_));
        pipeline_->remove_effect(pets::kHunterRefRanged, ability_seconds(pet_clock_));
        if (abilities_) take_ability_events();
    }
    hunter_stance_set_ = false;
    hunter_ranged_ = false;
    if (registry_ && pet_actor_) registry_->remove(pet_actor_);
    if (pet_actor_) pet_dirty_ = true;
    pet_actor_ = 0;
    pet_effect_ = -1;
}

void PlayerSession::dismiss_pet(Clock::time_point now) {
    if (pipeline_) {
        for (const auto& d : pets::kDefinitions) pipeline_->remove_effect(d.effect, ability_seconds(now));
        if (abilities_) take_ability_events();
    }
    pet_effects_.clear();
    clear_pet();
}

void PlayerSession::run_pets(Connection& conn, Clock::time_point now) {
    if (!world_ || !registry_ || !abilities_) return;
    pet_clock_ = now;
    const double sec = ability_seconds(now);
    const int64_t wanted = dead_ || pet_effects_.empty() ? -1 : pet_effects_.rbegin()->first;
    if (pet_actor_ && (!registry_->find(pet_actor_) || wanted != pet_effect_)) clear_pet();
    if (wanted >= 0 && !pet_actor_) {
        const pets::Definition* def = pets::for_effect(pet_effects_.at(wanted));
        const spawns::GenericNpcs* generic = world_->generic_npcs();
        content::NpcAssetRow asset;
        asset.archetype_path = def->archetype;
        const spawns::OpenTemplate* open = generic ? generic->template_for(asset) : nullptr;
        std::string error;
        std::optional<world::WorldActor> made;
        const auto motion = world_->blueprints().find(def->archetype);
        const double foot = motion ? motion->half_height * motion->default_scale : 88.0;
        if (open) {
            const GoodPosition at = checker_.good();
            const double yaw = at.yaw_degrees * 3.14159265358979323846 / 180.0;
            spawns::InstanceParams p;
            // SpawnNPCRecordBase.Location is a relative offset (200, 400, 0).
            p.x = at.x + 200.0 * std::cos(yaw) - 400.0 * std::sin(yaw);
            p.y = at.y + 200.0 * std::sin(yaw) + 400.0 * std::cos(yaw);
            p.z = at.z - 88.0 + foot;
            p.yaw_degrees = at.yaw_degrees;
            p.world_time = pb_.setup().world_time + session_seconds(now);
            p.server_id = world_->server_id();
            p.level = stats_ ? stats_->level() : 1;
            p.name = def->name;
            p.clear_population = true;
            made = spawns::instantiate(*open, [this] { return world_->mint_guid(); }, p, error);
        } else {
            error = "the pet Blueprint template is unavailable (load npc_motion.tsv and the area)";
        }
        if (made) {
            auto& actor = made->actor;
            actor.obj.state.set(*actor.obj.schema, "Instigator", repl::Value(pb_.setup().guids.pawn));
            made->never_dormant = true;
            made->npc.source = world::Source::Runtime;
            made->npc.aggro_off = true;
            const auto& words = pb_.setup().character_guid;
            for (auto& s : actor.subobjects) {
                if (!s.obj.schema) continue;
                auto set = [&](const char* path, repl::Value value) { return s.obj.state.set(*s.obj.schema, path, std::move(value)); };
                if (std::string(s.obj.schema->name) == "UNPCInfo") set("CreatureOwnerName", repl::Value::str(name()));
                if (std::string(s.obj.schema->name) == "UAlignmentComponent") {
                    // as retail's mounts have it [R: 911, 898, 871, 872]: it inherits what its owner is, and its Instigator row names its owner too.  Without that the client finds no
                    // relationship between the owner and the pet (ABaseCharacter::GetBaseCombatPermissionsBitmasks: "undefined", no permissions) [modelled for a pet: no recording opens one]
                    if (const repl::Subobject* own = pb_.pawn().find(pb_.setup().guids.alignment)) {
                        if (const repl::Value* v = own->obj.state.get(*own->obj.schema, "Alignments")) {
                            if (const auto* inherited = v->get_if<repl::AlignmentContainer>()) set("InheritedAlignments", repl::Value(*inherited));
                        }
                    }
                    const repl::Value* held = s.obj.state.get(*s.obj.schema, "Alignments");
                    repl::AlignmentContainer alignments = held && held->get_if<repl::AlignmentContainer>() ? *held->get_if<repl::AlignmentContainer>() : repl::AlignmentContainer{};
                    const repl::GuidValue owner{words[0], words[1], words[2], words[3]};
                    bool named = false;
                    for (auto& inst : alignments.instances) {
                        if (inst.key != bootstrap::kAlignmentKeyCharacter) continue;
                        inst.guids.push_back(owner);
                        named = true;
                    }
                    if (!named) alignments.instances.push_back({bootstrap::kAlignmentKeyCharacter, {owner}});
                    set("Alignments", repl::Value(std::move(alignments)));
                }
                if (std::string(s.obj.schema->name) == "UNPCSecondaryInfo") {
                    const char* paths[] = {"CreatureOwnerGuid.A", "CreatureOwnerGuid.B", "CreatureOwnerGuid.C", "CreatureOwnerGuid.D"};
                    for (size_t i = 0; i < 4; ++i) set(paths[i], repl::Value::i64(static_cast<int32_t>(words[i])));
                    set("PetSpawnerEffectId", repl::Value::i64(wanted));
                }
                if (std::string(s.obj.schema->name) == "UAoCAbilityComponent") {
                    // the client plays only abilities the component has loaded: the pet's attack joins its list, in the shape of the list's own elements
                    const repl::Value* v = s.obj.state.get(*s.obj.schema, "ReplicatedRecordsToLoad");
                    const auto* list = v ? v->get_if<repl::ArrayValue>() : nullptr;
                    if (list && !list->elements.empty()) {
                        repl::ArrayValue loaded;
                        const auto load = [&](int64_t record) {
                            repl::ElementState e = list->elements.front();
                            e.values[1] = repl::Value::i64(record);
                            loaded.elements.push_back(std::move(e));
                        };
                        load(def->attack);
                        if (def->effect == pets::kHunterEffect) load(pets::kHunterRangedAttack);
                        for (const pets::PetAbility& a : pets::kPetAbilities) {
                            if (a.pet_effect == def->effect && a.ability != pets::kHunterStanceSwap) load(a.ability);
                        }
                        set("ReplicatedRecordsToLoad", repl::Value(std::move(loaded)));
                    } else {
                        spdlog::warn("[pets] {}: {} has no list of ability records: its attacks do not animate", name(), def->name);
                    }
                }
            }
            const uint64_t id = made->id;
            if (registry_->add(std::move(*made))) {
                pet_actor_ = id;
                pet_effect_ = wanted;
                pet_dirty_ = true;
                pet_follow_ = true;
                pet_follow_at_ = sec + 0.5;
                if (combat_side_ && npc_world_ && npc_world_->combat()) npc_world_->combat()->pet_summoned(id, combat_side_->actor_id());
                fill_pet_bar(conn, *def);
                if (def->effect == pets::kHunterEffect) {
                    // it comes in its melee stance (the summon's own hit gives the reference; a Hunter replacing a Hunter has just lost it), and each Stance Swap changes it
                    pipeline_->grant_effect(pets::kHunterRefMelee, sec);
                    take_ability_events();
                    hunter_stance_set_ = true;
                }
                if (auto* npcs = npc_world_ ? npc_world_->npcs() : nullptr) {
                    npc::NpcMotionSetup setup;
                    setup.foot_offset = foot;
                    setup.home_yaw = checker_.good().yaw_degrees;
                    npcs->add(id, setup);
                }
                spdlog::info("[pets] {}: {} summoned (actor {}, effect {})", name(), def->name, id, wanted);
            } else {
                error = "the actor registry refused the pet";
            }
        }
        if (!pet_actor_) {
            spdlog::warn("[pets] {}: {} could not be summoned: {}", name(), def->name, error);
            // Do not leave the UI claiming a pet exists after a failed spawn.
            pipeline_->remove_effect(def->effect, sec);
            take_ability_events();
        }
    }
    if (pet_dirty_ && room_for(1)) {
        if (!interact_info_) interact_info_ = world_->mint_guid();
        const auto info = *interact_info_;
        const auto* pet = registry_->find(pet_actor_);
        const auto guid = pet ? pet->actor.guid : protocol::emit::FIntrepidNetworkGUID{};
        const int64_t effect = pet_effect_;
        queue("the pawn's combat pet", [info, guid, effect](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.ensure_interact_info(info)) return std::nullopt;
            return pb.pawn_combat_pet(guid, effect);
        }, conn, /*must_build=*/true);
        pet_dirty_ = false;
    }
    combat::CombatWorld* fights = npc_world_ ? npc_world_->combat() : nullptr;
    if (pet_actor_ && fights && fights->is_pet(pet_actor_) && fights->dead(pet_actor_)) {   // its Health ran out: it is gone, and it takes a new summon to bring it back
        spdlog::info("[pets] {}: its pet (actor {}) was killed", name(), pet_actor_);
        dismiss_pet(now);
        return;
    }
    if (pet_actor_ && fights) {
        if (const uint64_t incapacitated = fights->take_pet_stand_down(pet_actor_)) {
            fights->pet_stop(pet_actor_);
            pet_incap_target_ = incapacitated;
        }
        // (it lets go while the creature holds the effect; a crowd control is left off a creature, so with no effect on it the Hunter takes it up again at once)
        if (pet_incap_target_ != 0 && fights->effect_stacks(pet_incap_target_, pets::kHunterIncapMelee) == 0) pet_incap_target_ = 0;
    }
    // Defensive: what hurts its owner comes first, even when it is fighting something else (HandlePetOwnerOnHit's threat): it leaves a creature that is not on its owner for one that is
    if (pet_actor_ && pet_follow_ && pet_stance_ == pets::Stance::Defensive && fights && combat_side_ && fights->pet_fighting(pet_actor_)) {
        const uint64_t owner = combat_side_->actor_id();
        const uint64_t now_on = fights->pet_target(pet_actor_);
        if (now_on == 0 || fights->creature_target(now_on) != owner) {
            const uint64_t on_owner = fights->owner_attacker(owner, pet_actor_, 3000.0);
            const int64_t attack = pet_attack_ability();
            if (on_owner != 0 && on_owner != now_on && on_owner != pet_held_target_ && attack != 0 && fights->pet_attack(pet_actor_, owner, attack, on_owner)) {
                spdlog::info("[pets] {}: Defensive: the pet leaves actor {} for actor {}, which is attacking its owner", name(), now_on, on_owner);
            }
        }
    }
    // a following pet by itself, as the client's ANPCCharacter::HandlePetOwnerOnHit has it: Aggressive joins what its owner hits, Defensive what hurts its owner, Passive neither; whatever fights the pet itself, it answers
    if (pet_actor_ && pet_follow_ && pet_stance_ != pets::Stance::Passive && fights && combat_side_ && !fights->pet_fighting(pet_actor_)) {
        const uint64_t owner = combat_side_->actor_id();
        const int64_t attack = pet_attack_ability();
        uint64_t aim = 0;
        if (pet_stance_ == pets::Stance::Aggressive && active_targets_[0].is_valid()) {
            const uint64_t target = fights->actor_of_object(active_targets_[0].ObjectId);
            if (target && fights->fought_by(target, owner)) aim = target;
        }
        if (aim == 0) aim = fights->attacker_of(owner, pet_actor_, 3000.0, pet_stance_ == pets::Stance::Defensive);
        if (aim != 0 && aim != pet_held_target_ && aim != pet_incap_target_ && attack != 0) fights->pet_attack(pet_actor_, owner, attack, aim);
    }
    const bool busy = fights && (fights->pet_fighting(pet_actor_) || fights->pet_casting(pet_actor_));
    if (!pet_owner_at_) pet_owner_at_ = std::make_shared<nav::Vec3>();
    {
        const GoodPosition at = checker_.good();
        *pet_owner_at_ = nav::Vec3{at.x, at.y, at.z - 88.0};
    }
    if (busy || !pet_follow_) pet_chasing_owner_ = false;   // (whatever took its feet: it sets off again when that is over)
    if (pet_actor_ && pet_follow_ && !busy) {   // left far behind (a teleport, a mount): Summoner_Pet_LeashTeleport
        const auto* pet = registry_->find(pet_actor_);
        auto* npcs = npc_world_ ? npc_world_->npcs() : nullptr;
        if (pet && npcs && std::hypot(pet->x - pet_owner_at_->x, pet->y - pet_owner_at_->y) > pets::kLeashCm) {
            npcs->warp(pet_actor_, pet_owner_at_->x + 200.0, pet_owner_at_->y, pet_owner_at_->z);
            pet_chasing_owner_ = false;
            spdlog::info("[pets] {}: the pet was left behind: it comes back to its owner", name());
        }
    }
    if (pet_actor_ && pet_follow_ && !busy && !pet_chasing_owner_) {
        // it walks after its owner as a creature walks after its target: one walk the NPC world keeps up to date, not a new one every half second that ends and starts over
        if (auto* npcs = npc_world_ ? npc_world_->npcs() : nullptr) {
            const auto* pet = registry_->find(pet_actor_);
            if (pet && std::hypot(pet->x - pet_owner_at_->x, pet->y - pet_owner_at_->y) > 350.0) {
                const std::shared_ptr<nav::Vec3> owner = pet_owner_at_;
                pet_chasing_owner_ = npcs->order_pursue(pet_actor_, [owner](nav::Vec3& out) {
                    out = *owner;
                    return true;
                }, 720.0, 250.0);
            }
        }
    }
    run_pet_automation(conn, now);
}

void PlayerSession::fill_pet_bar(Connection& conn, const pets::Definition& def) {
    repl::ActionbarPayload payload;
    const auto put = [&](int slot_index, int64_t ability, int64_t command) {
        repl::ActionbarSlot slot;
        slot.bank_index = repl::ActionbarBank::Pet;
        slot.slot_index = slot_index;
        slot.use.use_type = repl::ActionbarUseType::PetCommand;
        // the Player, as the client's own drags have it: a slot is lit when its instigator's KnownAbilities hold the ability (UActionBarSlotBase's tick), and these are the owner's, auto-granted by the Summoner tree
        slot.use.instigator = repl::ActionbarInstigator::Player;
        slot.use.ability_id = ability;
        slot.use.pet_command_id = command;
        std::string why;
        if (bar_.set(slot, &why)) payload.slots.push_back(slot);
    };
    // the first three slots follow the pet: empty, or holding another pet's ability, they take this pet's; a slot the player filled with anything else is left alone
    int next = 0;
    for (const pets::PetAbility& a : pets::kPetAbilities) {
        if (a.pet_effect != def.effect || a.ability == pets::kHunterStanceSwap || next >= 3) continue;
        const repl::ActionbarUseData* held = bar_.at(repl::ActionbarBank::Pet, next);
        if (!held || held->ability_id == a.ability || pets::pet_of_ability(held->ability_id) != 0) {
            if (!held || held->ability_id != a.ability) put(next, a.ability, pets::bar_command_of(a.ability));
        }
        ++next;
    }
    // the commands go on the slots after them, once: a command the player has placed or removed since is theirs
    bool any_command = false;
    for (const repl::ActionbarSlot& s : bar_.slots()) {
        if (s.bank_index != repl::ActionbarBank::Pet) continue;
        for (const pets::PetBarCommand& c : pets::kPetBarCommands) any_command = any_command || s.use.ability_id == c.ability;
    }
    if (!any_command) {
        int slot_index = 3;
        for (const pets::PetBarCommand& c : pets::kPetBarCommands) {
            while (slot_index < 12 && bar_.at(repl::ActionbarBank::Pet, slot_index)) ++slot_index;
            if (slot_index >= 12) break;
            put(slot_index++, c.ability, c.command);
        }
    }
    // a slot saved with the pet as its instigator stays grey for good otherwise
    std::vector<repl::ActionbarSlot> saved;
    for (const repl::ActionbarSlot& s : bar_.slots()) {
        if (s.bank_index == repl::ActionbarBank::Pet && s.use.instigator == repl::ActionbarInstigator::Pet) saved.push_back(s);
    }
    for (repl::ActionbarSlot& s : saved) {
        s.use.instigator = repl::ActionbarInstigator::Player;
        std::string why;
        if (bar_.set(s, &why)) payload.slots.push_back(s);
    }
    if (payload.slots.empty()) return;
    payload.auto_granted = true;
    queue("ClientReceiveActionbarSlotPayload", [payload](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(payload); }, conn, /*must_build=*/true);
    progress_changed_ = true;
    hand_over_progress("the action bar");
    spdlog::info("[pets] {}: the pet bar gets {} slot(s) for {}", name(), payload.slots.size(), def.name);
}

void PlayerSession::run_pet_automation(Connection& conn, Clock::time_point now) {
    combat::CombatWorld* fights = npc_world_ ? npc_world_->combat() : nullptr;
    const pets::Definition* def = pet_definition();
    if (!pet_actor_ || !def || !fights || !combat_side_ || !pipeline_ || dead_) return;
    const double sec = ability_seconds(now);
    if (sec < pet_auto_at_) return;
    pet_auto_at_ = sec + 0.5;
    if (pipeline_->stacks_of(pets::kAutomationEffect) == 0 || fights->pet_casting(pet_actor_) || fights->dead(pet_actor_)) return;
    const uint64_t owner = combat_side_->actor_id();
    // what it would use a hostile ability on: what it fights, else what is fighting its owner or it; a Passive pet looks for nothing by itself
    uint64_t enemy = fights->pet_target(pet_actor_);
    if (enemy == 0 && pet_stance_ != pets::Stance::Passive) enemy = fights->attacker_of(owner, pet_actor_, 3000.0);
    if (pet_stance_ == pets::Stance::Defensive) {   // (what is on its owner is what its Taunt is for)
        if (const uint64_t on_owner = fights->owner_attacker(owner, pet_actor_, 3000.0)) enemy = on_owner;
    }
    const double owner_health = pipeline_->stat_of(content::kGuidHealth), owner_max = pipeline_->stat_of(content::kGuidMaxHealth);
    const double pet_health = fights->health_of(pet_actor_), pet_max = fights->max_health_of(pet_actor_);
    const bool at_max_essence = pipeline_->stat_of(pets::kEssence) >= pets::kEssenceMax - 0.01f;
    for (const pets::PetAbility& a : pets::kPetAbilities) {
        if (a.pet_effect != def->effect || a.rule == pets::AutoRule::Never) continue;
        if (const auto ready = pet_cooldowns_.find(a.ability); ready != pet_cooldowns_.end() && sec < ready->second) continue;
        uint64_t aim = 0;
        switch (a.rule) {
            case pets::AutoRule::NotMostHated:
                if (enemy != 0 && fights->creature_target(enemy) != pet_actor_) aim = enemy;
                break;
            case pets::AutoRule::AtMaxEssence:
                if (enemy != 0 && at_max_essence) aim = enemy;
                break;
            case pets::AutoRule::HealBelow80:
                if (owner_max > 0 && owner_health < 0.8 * owner_max) aim = owner;
                else if (pet_max > 0 && pet_health < 0.8 * pet_max) aim = pet_actor_;
                break;
            case pets::AutoRule::Cleanse:
                if (pipeline_->holds_dispellable()) aim = owner;
                else if (fights->holds_dispellable(pet_actor_)) aim = pet_actor_;
                break;
            default: break;
        }
        if (aim == 0) continue;
        pet_use_ability(conn, a.ability, aim, now);
        if (fights->pet_casting(pet_actor_)) return;   // one at a time; it looks again when that is over
    }
}

void PlayerSession::pet_use_ability(Connection& conn, int64_t ability, uint64_t target, Clock::time_point now, const char* by) {
    const pets::Definition* def = pet_definition();
    combat::CombatWorld* fights = npc_world_ ? npc_world_->combat() : nullptr;
    const abilities::AbilityDef* record = world_ && world_->ability_catalog() ? world_->ability_catalog()->ability(ability) : nullptr;
    const std::string what = record ? record->row.name : std::to_string(ability);
    const std::string asked = by ? by : "the automation";
    const auto refused = [&](const std::string& why) {
        spdlog::log(by ? spdlog::level::info : spdlog::level::debug, "[pets] {}: {} asked by {} is refused: {}", name(), what, asked, why);
    };
    if (dead_ || !pet_actor_ || !def || !pipeline_ || pets::pet_of_ability(ability) != def->effect) return refused("no pet of its kind");
    const double sec = ability_seconds(now);
    if (const auto ready = pet_cooldowns_.find(ability); ready != pet_cooldowns_.end() && sec < ready->second) return refused(fmt::format("its cooldown has {:.1f} s left", ready->second - sec));
    if (ability == pets::kHunterStanceSwap) {
        hunter_ranged_ = !hunter_ranged_;
        pipeline_->remove_effect(hunter_ranged_ ? pets::kHunterRefMelee : pets::kHunterRefRanged, sec);
        pipeline_->grant_effect(hunter_ranged_ ? pets::kHunterRefRanged : pets::kHunterRefMelee, sec);
        pipeline_->start_pet_cooldown(ability, 0.1, sec);
        take_ability_events();
        flush_abilities(conn);
        // the fight goes on with the other attack
        if (fights && combat_side_ && fights->pet_fighting(pet_actor_) && active_targets_[0].is_valid()) {
            const uint64_t aim = fights->actor_of_object(active_targets_[0].ObjectId);
            if (aim) fights->pet_attack(pet_actor_, combat_side_->actor_id(), pet_attack_ability(), aim);
        }
        pet_cooldowns_[ability] = sec + 0.1;
        spdlog::info("[pets] {}: the Hunter's stance is {} now", name(), hunter_ranged_ ? "ranged" : "melee");
        return;
    }
    if (!fights || !combat_side_) return refused("no fights in this world");
    uint64_t aim = target;
    if (aim == 0 && active_targets_[0].is_valid()) aim = fights->actor_of_object(active_targets_[0].ObjectId);
    if (!fights->pet_can_act(pet_actor_)) return refused("the pet cannot act");
    const double essence = pipeline_->stat_of(pets::kEssence);
    double cooldown = 0;
    if (!pipeline_->pay_pet_ability(ability, sec, cooldown)) {
        refused(fmt::format("the Essence is not there (the owner has {:.1f})", essence));
        take_ability_events();
        return;
    }
    take_ability_events();
    flush_abilities(conn);
    double reach = def->effect == pets::kHunterEffect && hunter_ranged_ ? pets::kHunterRangedReach : def->reach_cm;
    if (ability == pets::kGuardianTaunt) reach = pets::kGuardianTauntReach;
    if (ability == pets::kHunterLunge && !hunter_ranged_) reach = 999.0;   // (it leaps from here: Lunge's distance comes with the leap)
    if (!fights->pet_use(pet_actor_, combat_side_->actor_id(), ability, aim, reach)) {
        spdlog::warn("[pets] {}: {} asked by {} was paid for and the pet cannot play it", name(), what, asked);
        return;
    }
    pet_cooldowns_[ability] = sec + cooldown;
    // the cooldown shows on the pet's ability component [modelled: no recording shows a pet's Cooldowns]
    if (cooldown >= 1.0 && registry_) {
        const double start = sync_now(now);
        bootstrap::CooldownItem item;
        item.ability_guid = ability;
        item.triggered = true;
        item.initial_cooldown = static_cast<float>(cooldown);
        item.start_time = start;
        item.end_time = start + cooldown;
        registry_->modify(pet_actor_, world::Change::State, [&](world::WorldActor& a) {
            for (auto& s : a.actor.subobjects) {
                if (!s.obj.schema || std::string(s.obj.schema->name) != "UAoCAbilityComponent") continue;
                repl::FastArrayState* arr = s.obj.arrays.get(*s.obj.schema, "Cooldowns");
                if (!arr) continue;
                const auto held = pet_cooldown_items_.find(ability);
                if (held != pet_cooldown_items_.end()) {
                    arr->change(held->second, bootstrap::cooldown_item_state(item));
                } else if (const auto id = arr->add(bootstrap::cooldown_item_state(item))) {
                    pet_cooldown_items_[ability] = *id;
                }
            }
        });
    }
    spdlog::info("[pets] {}: {} asked by {} at actor {}: Essence {:.1f} -> {:.1f}, cooldown {:.1f} s, reach {:.0f} cm", name(), what, asked, aim, essence, pipeline_->stat_of(pets::kEssence), cooldown, reach);
}

void PlayerSession::pet_to_bolt_target(Connection& conn, uint64_t target, bool behind, Clock::time_point now) {
    const world::WorldActor* prey = registry_ ? registry_->find(target) : nullptr;
    const world::WorldActor* pet = registry_ && pet_actor_ ? registry_->find(pet_actor_) : nullptr;
    npc::NpcWorld* npcs = npc_world_ ? npc_world_->npcs() : nullptr;
    if (!prey || !pet || !npcs) return;
    double dx = pet->x - prey->x, dy = pet->y - prey->y;
    if (behind) {
        const double yaw = prey->yaw_degrees * 3.14159265358979323846 / 180.0;
        dx = -std::cos(yaw);
        dy = -std::sin(yaw);
    }
    const double length = std::hypot(dx, dy);
    if (length < 1.0) {
        dx = 1.0;
        dy = 0.0;
    } else {
        dx /= length;
        dy /= length;
    }
    pet_chasing_owner_ = false;
    npcs->warp(pet_actor_, prey->x + dx * pets::kBoltArrivalCm, prey->y + dy * pets::kBoltArrivalCm, prey->z);
    pet_command(conn, pets::kAttack, target, std::nullopt, now);
    spdlog::info("[pets] {}: Dimensional Bolt sends the pet to actor {}", name(), target);
}

void PlayerSession::pet_joins_owners_cast(uint64_t target) {
    combat::CombatWorld* fights = npc_world_ ? npc_world_->combat() : nullptr;
    if (!pet_actor_ || !pet_follow_ || pet_stance_ != pets::Stance::Aggressive || !fights || !combat_side_ || fights->pet_fighting(pet_actor_) || target == pet_held_target_) return;
    const int64_t attack = pet_attack_ability();
    if (attack != 0) fights->pet_attack(pet_actor_, combat_side_->actor_id(), attack, target);   // (an ally the owner aims at: it refuses)
}

void PlayerSession::on_pet_command(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_ || !pet_actor_ || !rpc.call.decoded || !within_budget(quiet_budget_, "ServerSendPetCommand", now)) return;
    const auto command = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "PetCommandId"), 0);
    if (!command) return;
    std::optional<nav::Vec3> place;
    const repl::Value* has = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "TargetData.bHasLocation");
    const repl::Value* where = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "TargetData.TargetLocation");
    const bool* has_place = has ? has->get_if<bool>() : nullptr;
    const repl::Vec3d* at = where ? where->get_if<repl::Vec3d>() : nullptr;
    if (has_place && *has_place && at) place = nav::Vec3{at->x, at->y, at->z};
    // the abilities of a pet's bar: the call names the ability [the schema's AbilityId; no recording shows this call]
    if (*command == pets::kUseAbility || *command == pets::kUseAbilityNonCombat || *command == pets::kUseAbilityNonTarget) {
        const auto ability = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "AbilityId"), 0);
        if (ability && *ability) pet_use_ability(conn, *ability, 0, now, "the bar (a pet command)");
        return;
    }
    pet_command(conn, *command, 0, place, now);
}

void PlayerSession::set_pet_stance(Connection& conn, pets::Stance stance) {
    pet_stance_ = stance;
    if (!registry_ || !pet_actor_) return;
    if (!interact_info_) interact_info_ = world_->mint_guid();
    const auto info = *interact_info_;
    queue("the pawn's pet stance", [info, stance](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.ensure_interact_info(info)) return std::nullopt;
        return pb.pawn_pet_stance(static_cast<int64_t>(stance));
    }, conn, /*must_build=*/true);
    registry_->modify(pet_actor_, world::Change::State, [&](world::WorldActor& a) {
        for (auto& s : a.actor.subobjects) {
            if (s.obj.schema && std::string(s.obj.schema->name) == "UNPCSecondaryInfo") s.obj.state.set(*s.obj.schema, "PetStance", repl::Value::u64(static_cast<uint64_t>(stance)));
        }
    });
}

void PlayerSession::pet_command(Connection& conn, int64_t command, uint64_t target, const std::optional<nav::Vec3>& place, Clock::time_point now) {
    if (dead_ || !pet_actor_) return;
    combat::CombatWorld* fights = npc_world_ ? npc_world_->combat() : nullptr;
    npc::NpcWorld* npcs = npc_world_ ? npc_world_->npcs() : nullptr;
    const uint64_t active = fights && active_targets_[0].is_valid() ? fights->actor_of_object(active_targets_[0].ObjectId) : 0;
    const auto call_off = [&] {
        if (fights && fights->pet_fighting(pet_actor_)) {
            pet_held_target_ = active;
            fights->pet_stop(pet_actor_);
        }
    };
    if (command == pets::kDismiss) {
        dismiss_pet(now);
        run_pets(conn, now);
    } else if (command == pets::kAttack) {
        const uint64_t aim = target != 0 ? target : active;
        const int64_t attack = pet_attack_ability();
        pet_held_target_ = 0;
        const bool going = fights && combat_side_ && attack && aim && fights->pet_attack(pet_actor_, combat_side_->actor_id(), attack, aim);
        spdlog::info("[pets] {}: Attack on actor {}: {}", name(), aim, going ? "the pet goes" : "no target it can attack");
    } else if (command == pets::kFollow) {
        call_off();
        pet_follow_ = true;
        pet_follow_at_ = 0;
    } else if (command == pets::kStay) {
        call_off();
        pet_follow_ = false;
        if (npcs) npcs->halt(pet_actor_);
    } else if (command == pets::kGoTo) {
        call_off();
        pet_follow_ = false;
        // the cast names the place it was aimed at; failing that, it goes to the target its owner holds
        const world::WorldActor* aimed = !place && active && registry_ ? registry_->find(active) : nullptr;
        const std::optional<nav::Vec3> where = place ? place : aimed ? std::optional<nav::Vec3>(nav::Vec3{aimed->x, aimed->y, aimed->z}) : std::nullopt;
        if (npcs && where) npcs->order_move(pet_actor_, *where, 720.0, 100.0);
        else spdlog::info("[pets] {}: Go To names no place: the pet stays", name());
    } else if (command == pets::kAggressive) {
        set_pet_stance(conn, pets::Stance::Aggressive);
    } else if (command == pets::kDefensive) {
        set_pet_stance(conn, pets::Stance::Defensive);
    } else if (command == pets::kPassive) {
        call_off();
        set_pet_stance(conn, pets::Stance::Passive);
    } else {
        spdlog::info("[pets] {}: pet command {} is not served", name(), command);
    }
}

void PlayerSession::drain_pet(Connection& conn, Clock::time_point now) {
    const double sec = ability_seconds(now);
    std::vector<combat::DealtHit> dealt;
    dealt.swap(combat_side_->dealt);
    for (const combat::DealtHit& h : dealt) pipeline_->confirm_dealt(h, sec);
    std::vector<std::pair<int64_t, int64_t>> pet_hits;
    pet_hits.swap(combat_side_->pet_hits);
    for (const auto& [hit, ability] : pet_hits) pipeline_->land_pet_hit(hit, ability, sec);
    if (!dealt.empty() || !pet_hits.empty()) {
        take_ability_events();
        flush_abilities(conn);
    }
    if (const auto recall = pipeline_->take_bolt_recall(); recall.target != 0) pet_to_bolt_target(conn, recall.target, recall.behind, now);
    // Transpose: the two change places
    if (const uint64_t other = pipeline_->take_transpose()) {
        const world::WorldActor* a = registry_ ? registry_->find(other) : nullptr;
        auto* npcs = npc_world_ ? npc_world_->npcs() : nullptr;
        if (a && npcs) {
            const GoodPosition me = checker_.good();
            const double x = a->x, y = a->y, z = a->z;
            npcs->warp(other, me.x, me.y, me.z - 88.0);
            move_to(conn, x, y, z + 88.0, now, "Transpose", false);
        }
    }
}

bool PlayerSession::pet_takes_trigger(Connection& conn, int64_t ability, int32_t instance, const TriggerInfo& info, Clock::time_point now) {
    // The auto mode, cast from the bar: it turns on and off.  (Its hit takes the effect off before it asks whether the effect is off, so through the pipeline it could never turn off.)
    if (ability == pets::kAutomationAbility && pipeline_) {
        const double sec = ability_seconds(now);
        const bool was_on = pipeline_->stacks_of(pets::kAutomationEffect) > 0;
        if (was_on) pipeline_->remove_effect(pets::kAutomationEffect, sec);
        else pipeline_->grant_effect(pets::kAutomationEffect, sec);
        take_ability_events();
        flush_abilities(conn);
        end_instance(conn, instance, false);
        spdlog::info("[pets] {}: pet automation is {}", name(), was_on ? "off" : "on");
        return true;
    }
    // A pet's ability is the pet's to play: the owner's character casts nothing and pays its Essence (the Stance Swap comes this way [R: 476, 477])
    if (pets::pet_of_ability(ability) != 0) {
        pet_use_ability(conn, ability, info.target_actors.empty() ? 0 : info.target_actors.front(), now, "the bar (a trigger)");
        end_instance(conn, instance, false);
        return true;
    }
    return false;
}

void PlayerSession::register_pets_rpcs(RpcTable& t) {
    t.add(RpcScope::Pawn, "ServerSendPetCommand", [](PlayerSession& s, const RpcContext& c) { s.on_pet_command(c.conn, c.rpc, c.now); });
}

}  // namespace aoc::worldd

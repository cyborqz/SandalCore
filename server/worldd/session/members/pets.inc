// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/pets.inc: the members of PlayerSession's Summoner pet (session/player_pets.cpp)
//  Included at the end of the class (session/members_registered.inc).
// ============================================================================
public:
    uint64_t summoned_pet() const { return pet_actor_; }
    bool     pet_following() const { return pet_follow_; }
    pets::Stance pet_stance() const { return pet_stance_; }
    /// The attack of the pet in the world; 0 for none.
    int64_t  pet_attack_ability() const {
        const pets::Definition* def = pet_definition();
        if (def && def->effect == pets::kHunterEffect && hunter_ranged_) return pets::kHunterRangedAttack;
        return def ? def->attack : 0;
    }
    const pets::Definition* pet_definition() const {
        const auto it = pet_effects_.find(pet_effect_);
        return it == pet_effects_.end() ? nullptr : pets::for_effect(it->second);
    }
    bool hunter_ranged() const { return hunter_ranged_; }
    bool pet_automation() const { return pipeline_ && pipeline_->stacks_of(pets::kAutomationEffect) > 0; }

private:
    void run_pets(Connection& conn, Clock::time_point now);
    void clear_pet();
    void dismiss_pet(Clock::time_point now);
    void on_pet_command(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// What the world and the pipeline left for the session: the confirmations of the pet's hits, its hits on its owner, Dimensional Bolt's second press and Transpose.
    void drain_pet(Connection& conn, Clock::time_point now);
    /// A trigger that is the pet's: the auto mode's toggle, or one of a pet's abilities (the owner's character casts nothing).  True when it took the trigger.
    bool pet_takes_trigger(Connection& conn, int64_t ability, int32_t instance, const TriggerInfo& info, Clock::time_point now);
    /// A command to the pet, by ServerSendPetCommand or by the ability that names it. `target`: the actor the cast names (0: the active target). `place`: where a Go To goes.
    void pet_command(Connection& conn, int64_t command, uint64_t target, const std::optional<nav::Vec3>& place, Clock::time_point now);
    void set_pet_stance(Connection& conn, pets::Stance stance);
    /// The pet plays one of its abilities: the owner pays, the pet goes to it.  `by`: who asked, for the log (null: the auto mode).
    void pet_use_ability(Connection& conn, int64_t ability, uint64_t target, Clock::time_point now, const char* by = nullptr);
    /// Dimensional Bolt's second press: the pet appears at the marked creature and attacks it.
    void pet_to_bolt_target(Connection& conn, uint64_t target, bool behind, Clock::time_point now);
    /// An Aggressive pet joins what its owner casts at (ANPCCharacter::HandlePetOwnerTriggerAbility).
    void pet_joins_owners_cast(uint64_t target);
    /// The pet bar (bank 16): the commands, and in its first slots the abilities of the pet in the world.
    void fill_pet_bar(Connection& conn, const pets::Definition& def);
    /// The pet's auto mode: each ability by its rule (pets::AutoRule).
    void run_pet_automation(Connection& conn, Clock::time_point now);

    std::map<int64_t, int64_t>     pet_effects_;               // effect instance -> summon effect record
    uint64_t                       pet_actor_ = 0;
    int64_t                        pet_effect_ = -1;
    bool                           pet_dirty_ = false, pet_follow_ = true;
    double                         pet_follow_at_ = 0;
    uint64_t                       pet_held_target_ = 0;       // the target the pet was called off: it does not go back to it by itself
    uint64_t                       pet_incap_target_ = 0;      // what the Hunter just went to incapacitate: it leaves it alone while that holds
    pets::Stance                   pet_stance_ = pets::Stance::Aggressive;
    std::map<int64_t, double>      pet_cooldowns_;             // a pet's ability -> when it can be used again
    std::map<int64_t, int32_t>     pet_cooldown_items_;        // ... and its item in the pet's Cooldowns
    bool                           hunter_ranged_ = false, hunter_stance_set_ = false;
    double                         pet_auto_at_ = 0;           // when the auto mode looks next
    bool                           pet_chasing_owner_ = false; // the NPC world has it walking after its owner (it re-plans by itself)
    std::shared_ptr<nav::Vec3>     pet_owner_at_;              // where the owner stands, for that walk
    Clock::time_point              pet_clock_{};               // run_pets' last time

private:

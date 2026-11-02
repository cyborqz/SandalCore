// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_summoner_tests.inc: a Summoner's pet in a real session
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

static const repl::ArrayValue* summoned_pet_array(const PlayerSession& s) {
    for (const auto& component : s.bootstrap().pawn().subobjects) {
        if (component.name != "InteractInfo" || !component.obj.schema) continue;
        const auto* value = component.obj.state.get(*component.obj.schema, "PetArray");
        return value ? value->get_if<repl::ArrayValue>() : nullptr;
    }
    return nullptr;
}

void test_summoner_pet_session() {
    std::printf("Summoner pets: cast completion creates and replicates the owned pet\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    const auto prepare = [](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    };
    Run r(0, true, &rec, prepare);
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, false);
    uint64_t previous = 0;
    // A summon costs 150 x ManaCostMult (853 Mana at level 25), more than a Summoner has left after one.
    const auto refill_mana = [&r] { r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]); };
    for (const auto& d : pets::kDefinitions) {
        r.h.s->teach_ability(d.ability);
        refill_mana();
        r.trigger(d.ability);
        r.play(7.0, false);
        CHECK(r.h.s->summoned_pet() == previous, "a new pet is not created before completion");
        r.play(1.8, false);
        const uint64_t id = r.h.s->summoned_pet();
        const auto* pet = r.h.fx.world->registry().find(id);
        CHECK(id && id != previous && pet, "completion creates the pet actor");
        CHECK(!previous || !r.h.fx.world->registry().find(previous), "replacement removes the previous pet");
        if (!pet) return;
        CHECK(pet->npc.archetype == d.archetype && pet->npc.level == 25 && pet->never_dormant,
              "the pet has the right Blueprint and owner level and stays awake");
        const auto& object = pet->actor.obj;
        const auto* instigator = object.state.get(*object.schema, "Instigator");
        CHECK(instigator && instigator->get_if<emit::FIntrepidNetworkGUID>() &&
              *instigator->get_if<emit::FIntrepidNetworkGUID>() == r.h.s->bootstrap().setup().guids.pawn,
              "the pet names its summoner as instigator");
        const auto* array = summoned_pet_array(*r.h.s);
        CHECK(array && array->elements.size() == 1, "the player's replicated PetArray contains one pet");
        if (array && array->elements.size() == 1) {
            const auto& values = array->elements[0].values;
            const auto ptr = values.find(1);
            CHECK(ptr != values.end() && ptr->second.get_if<emit::FIntrepidNetworkGUID>() &&
                  *ptr->second.get_if<emit::FIntrepidNetworkGUID>() == pet->actor.guid, "PetArray points to the spawned actor");
        }
        CHECK(r.m.has_effect(d.effect), "the client received the pet's summon effect");
        {   // the client lights a slot only if its instigator knows the ability, and these are the owner's
            size_t on_bar = 0, the_players = 0;
            for (const repl::ActionbarSlot& s : r.h.s->action_bar().slots()) {
                if (s.bank_index != repl::ActionbarBank::Pet) continue;
                ++on_bar;
                if (s.use.instigator == repl::ActionbarInstigator::Player) ++the_players;
            }
            CHECK(on_bar > 0 && the_players == on_bar, "every slot of the pet bar names the Player as its instigator");
        }
        {   // what the client needs to find a relationship between the owner and the pet
            const repl::AlignmentContainer* inherited = nullptr;
            const repl::AlignmentContainer* own = nullptr;
            for (const auto& s : pet->actor.subobjects) {
                if (!s.obj.schema || std::string(s.obj.schema->name) != "UAlignmentComponent") continue;
                const auto* i = s.obj.state.get(*s.obj.schema, "InheritedAlignments");
                const auto* a = s.obj.state.get(*s.obj.schema, "Alignments");
                inherited = i ? i->get_if<repl::AlignmentContainer>() : nullptr;
                own = a ? a->get_if<repl::AlignmentContainer>() : nullptr;
            }
            const repl::Subobject* owner_alignment = r.h.s->bootstrap().pawn().find(r.h.s->bootstrap().setup().guids.alignment);
            const repl::Value* owner_value = owner_alignment ? owner_alignment->obj.state.get(*owner_alignment->obj.schema, "Alignments") : nullptr;
            const auto* owner_set = owner_value ? owner_value->get_if<repl::AlignmentContainer>() : nullptr;
            CHECK(inherited && owner_set && *inherited == *owner_set, "the pet inherits its owner's alignments");
            const auto& words = r.h.s->bootstrap().setup().character_guid;
            const repl::GuidValue owner{words[0], words[1], words[2], words[3]};
            bool names_owner = false;
            if (own) {
                for (const auto& inst : own->instances) {
                    if (inst.key == bootstrap::kAlignmentKeyCharacter) names_owner = names_owner || std::find(inst.guids.begin(), inst.guids.end(), owner) != inst.guids.end();
                }
            }
            CHECK(names_owner, "the pet's Instigator row names its owner");
        }
        CHECK(r.h.s->actor_view()->channel_of(id) != 0, "the pet opened on an actor channel for the client");
        previous = id;
    }
    r.h.s->set_current_value(kManaStat, 10.0f);
    r.trigger(pets::kDefinitions[0].ability);
    r.play(8.8, false);
    CHECK(r.h.s->summoned_pet() == previous && r.h.fx.world->registry().find(previous), "a summon the Summoner cannot pay for leaves the current pet");
    // the session knows which pet it has: what its Attack and its joining a fight read
    CHECK(r.h.s->pet_attack_ability() == pets::kDefinitions[2].attack, "the session finds the attack of the pet in the world");
    // the commands, cast from the bar as the client does: each ability names its command (PetCommandId)
    const auto cast = [&](int64_t ability) {
        r.h.s->teach_ability(ability);
        r.trigger(ability);
        r.play(0.3, false);
    };
    CHECK(r.h.s->pet_following() && r.h.s->pet_stance() == pets::Stance::Aggressive, "a new pet follows, aggressive");
    // Go To cast with no place: the pet stops following and stays, and a pet's ability is the pet's: a cast that belongs to another pet does nothing
    const float essence = r.m.stats.count(6064633668368219158ll) ? r.m.stats[6064633668368219158ll] : 0.0f;
    r.trigger(6064633577427633586ll);   // Stomp: a Guardian's, and this is a Mystic
    r.play(0.3, false);
    const float after_refusal = r.m.stats.count(6064633668368219158ll) ? r.m.stats[6064633668368219158ll] : 0.0f;
    CHECK(after_refusal == essence, "another pet's ability costs nothing");
    cast(6064633576956297816ll);   // Summoner_PetCommand_Stay
    CHECK(!r.h.s->pet_following(), "Stay, cast: the pet no longer follows");
    cast(6064633576846066210ll);   // Summoner_PetCommand_Follow
    CHECK(r.h.s->pet_following(), "Follow, cast: it follows again");
    cast(6064633724850470923ll);   // Summoner_PetCommand_PassiveStance
    const auto owner_stance = [&]() -> int64_t {
        for (const auto& component : r.h.s->bootstrap().pawn().subobjects) {
            if (component.name != "InteractInfo" || !component.obj.schema) continue;
            const auto* value = component.obj.state.get(*component.obj.schema, "PetStance");
            const uint64_t* stance = value ? value->get_if<uint64_t>() : nullptr;
            return stance ? static_cast<int64_t>(*stance) : -1;
        }
        return -1;
    };
    CHECK(r.h.s->pet_stance() == pets::Stance::Passive && owner_stance() == 3, "the Passive stance, cast, is on the owner's InteractInfo");
    cast(6064633724849553418ll);   // Summoner_PetCommand_DefensiveStance
    CHECK(r.h.s->pet_stance() == pets::Stance::Defensive && owner_stance() == 2, "the Defensive stance, cast, is on the owner's InteractInfo");
    cast(6064633724848504841ll);   // Summoner_PetCommand_AssistStance
    CHECK(r.h.s->pet_stance() == pets::Stance::Aggressive && owner_stance() == 1, "the Aggressive stance, cast, is on the owner's InteractInfo");
    CHECK(r.h.s->summoned_pet() == previous, "the commands leave the pet in the world");

    repl::RpcParams params;
    const auto& pawn = class_named("PlayerPawn_C");
    const auto* command = pawn.rpc("ServerSendPetCommand");
    CHECK(command && repl::set_rpc_param(*command, params, "PetCommandId", repl::Value::i64(pets::kDismiss)), "the dismiss command writes");
    r.send_reliable(actor_bunch(pawn, "ServerSendPetCommand", params));
    r.play(0.5, false);
    CHECK(!r.h.s->summoned_pet() && !r.h.fx.world->registry().find(previous), "dismiss removes the pet from the world");
    const auto* array = summoned_pet_array(*r.h.s);
    CHECK(array && array->elements.empty(), "dismiss clears the player's replicated PetArray");
    CHECK(!r.m.has_effect(pets::kDefinitions[2].effect), "dismiss clears the client's summon effect");

    refill_mana();
    r.trigger(pets::kDefinitions[0].ability);
    r.play(8.8, false);
    const uint64_t final_pet = r.h.s->summoned_pet();
    CHECK(final_pet != 0, "a dismissed pet can be summoned again");
    r.h.s->finish("pet cleanup test");
    CHECK(!r.h.fx.world->registry().find(final_pet), "logout removes the summoned pet");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "all owner replication updates read successfully");

    Run death(0, true, &rec, prepare);
    death.h.s->teach_ability(pets::kDefinitions[0].ability);
    death.play(0.5, false);
    death.trigger(pets::kDefinitions[0].ability);
    death.play(8.8, false);
    const uint64_t dead_pet = death.h.s->summoned_pet();
    CHECK(dead_pet != 0, "the death test starts with a summoned pet");
    death.h.s->take_damage(death.h.conn, 1.0e9, death.h.t);
    death.play(0.5, false);
    CHECK(!death.h.s->summoned_pet() && !death.h.fx.world->registry().find(dead_pet), "the owner's death removes the pet");
    CHECK(!death.m.has_effect(pets::kDefinitions[0].effect), "the owner's death clears the summon effect");
}

void test_summoner_hunter_stance() {
    std::printf("Summoner pets: the Hunter's stance swaps with its attack\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    const auto prepare = [](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    };
    Run r(0, true, &rec, prepare);
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, false);
    const pets::Definition& hunter = pets::kDefinitions[1];
    r.h.s->teach_ability(hunter.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(hunter.ability);
    r.play(8.8, false);
    CHECK(r.h.s->summoned_pet() != 0, "the Hunter comes");
    CHECK(r.h.s->pet_attack_ability() == hunter.attack && !r.h.s->hunter_ranged(), "it comes in melee");
    CHECK(r.m.has_effect(pets::kHunterRefMelee), "its owner holds the melee reference");
    r.h.s->teach_ability(pets::kHunterStanceSwap);
    r.trigger(pets::kHunterStanceSwap);
    r.play(0.3, false);
    CHECK(r.h.s->hunter_ranged() && r.h.s->pet_attack_ability() == pets::kHunterRangedAttack, "the swap gives it its ranged attack");
    CHECK(r.m.has_effect(pets::kHunterRefRanged) && !r.m.has_effect(pets::kHunterRefMelee), "the reference follows");
    r.trigger(pets::kHunterStanceSwap);
    r.play(0.3, false);
    CHECK(!r.h.s->hunter_ranged() && r.h.s->pet_attack_ability() == hunter.attack, "a second swap brings it back");
}

void test_summoner_pet_ability_session() {
    std::printf("Summoner pets: an ability cast from the bar pays Essence and the pet plays it\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld& world) {
        ww.prepare(world);
        std::string error;
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    });
    CHECK(ww.ground && r.h.fx.world->combat() != nullptr, "(a world with fights)");
    if (!ww.ground || !r.h.fx.world->combat()) return;
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, false);
    const pets::Definition& hunter = pets::kDefinitions[1];
    r.h.s->teach_ability(hunter.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(hunter.ability);
    r.play(8.8, false);
    CHECK(r.h.s->summoned_pet() != 0, "the Hunter comes");
    const int64_t kEssence = 6064633668368219158ll;
    const int64_t eviscerate = 6064633577430779319ll;
    r.h.s->set_current_value(kEssence, 100.0f);
    r.play(0.2, false);
    const float before = r.m.stats[kEssence];
    const float mana = r.m.stats[kManaStat];
    r.trigger(eviscerate);
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[kEssence] - (before - 30.0f)) < 2.0f, "Eviscerate takes 30 Essence");
    CHECK(r.m.stats[kManaStat] >= mana, "the character pays no Mana for its pet's ability");
    // the pet bar fills as the pet comes: its three abilities in the first slots, the commands after them
    const auto& bar = r.h.s->action_bar();
    const auto* first = bar.at(repl::ActionbarBank::Pet, 0);
    CHECK(first && first->use_type == repl::ActionbarUseType::PetCommand && pets::pet_of_ability(first->ability_id) == hunter.effect, "the pet bar's first slot holds one of the Hunter's abilities");
    bool has_follow = false;
    for (int i = 0; i < 12; ++i) {
        const auto* s = bar.at(repl::ActionbarBank::Pet, i);
        has_follow = has_follow || (s && s->pet_command_id == pets::kFollow);
    }
    CHECK(has_follow, "... and Follow is on it");
    r.play(3.0, false);
    r.h.s->set_current_value(kEssence, 5.0f);
    r.play(0.2, false);
    const float little = r.m.stats[kEssence];
    r.trigger(eviscerate);
    r.play(0.3, false);
    CHECK(r.m.stats[kEssence] >= little, "with too little Essence nothing is paid");
    r.play(3.0, false);
    // the auto mode: off, the pet uses nothing by itself; cast from the bar it turns on, and a second cast turns it off
    CHECK(!r.h.s->pet_automation(), "the auto mode starts off");
    r.h.s->teach_ability(pets::kAutomationAbility);
    r.trigger(pets::kAutomationAbility);
    r.play(0.2, false);
    CHECK(r.h.s->pet_automation() && r.m.has_effect(pets::kAutomationEffect), "cast, it turns on");
    r.trigger(pets::kAutomationAbility);
    r.play(0.2, false);
    CHECK(!r.h.s->pet_automation() && !r.m.has_effect(pets::kAutomationEffect), "cast again, it turns off");
    // a Mystic in auto mode heals its owner below 80% Health, by the rule in the data, and leaves one above it alone
    const pets::Definition& mystic = pets::kDefinitions[2];
    r.h.s->teach_ability(mystic.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(mystic.ability);
    r.play(8.8, false);
    CHECK(r.h.s->pet_definition() == &mystic, "(the Mystic comes)");
    r.trigger(pets::kAutomationAbility);
    r.h.s->set_current_value(kEssence, 80.0f);
    r.play(1.2, false);
    const float rested = r.m.stats[kEssence];
    CHECK(rested >= 79.0f, "at full Health it spends nothing");
    r.h.s->set_current_value(content::kGuidHealth, r.m.stats[content::kGuidMaxHealth] * 0.5f);
    r.play(1.0, false);
    std::printf("  [info] auto Recreate: Essence %.1f -> %.1f\n", rested, r.m.stats[kEssence]);
    CHECK(r.m.stats[kEssence] < rested - 10.0f, "below 80% Health the Mystic casts Recreate by itself");
}

void test_summoner_spawn_failure() {
    const svc::Character rec = class25(6, "Summoner", 17753);
    Run r(0, true, &rec);
    const auto& d = pets::kDefinitions[0];
    r.h.s->teach_ability(d.ability);
    r.play(0.5, false);
    r.trigger(d.ability);
    r.play(8.8, false);
    CHECK(!r.h.s->summoned_pet() && !r.m.has_effect(d.effect), "a missing pet template leaves no phantom pet effect");
    CHECK(!r.h.s->abilities_failed(), "a missing pet template does not disable abilities");
}
AOC_REGISTER_TEST(test_summoner_pet_session);
AOC_REGISTER_TEST(test_summoner_hunter_stance);
AOC_REGISTER_TEST(test_summoner_pet_ability_session);
AOC_REGISTER_TEST(test_summoner_spawn_failure);

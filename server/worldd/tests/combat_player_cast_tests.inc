// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_player_cast_tests.inc
//
//  Registered tests of test_worldd_combat: a player's cast through the ability runtime on an NPC, and the picks of the weapon finishers.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

void test_player_cast(Fixture& f) {
    std::printf("a player's cast and its hits on an NPC\n");
    Rig rig(4, 20, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 600, 0, 8, 100);   // (a small aggro radius: it does not come for the player in this test)
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    rig.know({"WeaponCombo_Melee_1H_Axe", "Bard_Misc_Flourish"});
    const abilities::AbilityDef* axe = f.catalog->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && wolf != 0, "the 1H axe combo and a Wolf 6 m away");
    if (!axe || wolf == 0) return;
    const int64_t ability = axe->row.guid;
    const abilities::HitDef* hit1 = f.catalog->hit(axe->hits[0].hit_guid);
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
// [C.O.A x C_Y_B]

    // a cast that names the Wolf: accepted; the ability item's Targets is the Wolf's collision cylinder
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    double t = 1.0;
    CHECK(rig.pipe->trigger(ability, 10, t, info) == Refusal::None, "a cast at the Wolf is accepted");
    std::vector<Seen> seen = rig.run_to(t + 0.4);
    const Seen* item = nullptr;
    for (const Seen& s : seen) {
        if (!s.change.abilities_added.empty()) item = &s;
    }
    CHECK(item != nullptr, "its ability item comes");
    if (item) {
        const bootstrap::ActiveAbilityItem& it = item->change.abilities_added[0];
        CHECK(!it.targets_self && it.target_objects.size() == 1 && it.target_objects[0].ObjectId == wolf + 4, "the item's Targets is the Wolf's collision cylinder (the actor's + 4, inside its block: E1 stage 3 fix 3), not the caster's");
    }

    // the client's hit verification: the request names the Wolf's collision cylinder
    t += 0.5;
    const auto verdict = rig.pipe->hit_request(ability, targeting, hit1->row.guid, 1, 0, t, {wolf});
    CHECK(verdict == AbilityPipeline::HitVerdict::Scheduled, "the hit request is scheduled");
    seen = rig.run_to(t + 0.5);
    const bootstrap::HitConfirm* conf = nullptr;
    for (const Seen& s : seen) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid == hit1->row.guid && !c.targets.empty()) conf = &c;
        }
    }
    CHECK(conf != nullptr, "the hit is confirmed with its targets");
    if (conf) {
        // E1 stage 3 fix 2: the HitComp is the Wolf ACTOR [R: research/hitfx 1, 7,450 retail targets]; its collision cylinder (the actor's id + 4, retail's slot
        // inside the block: E1 stage 3 fix 3) rides as the export
        CHECK(conf->targets.size() == 1 && conf->targets[0].component.ObjectId == wolf && conf->targets[0].cylinder.ObjectId == wolf + 4 && conf->targets[0].has_impact &&
                  conf->mods.empty() && conf->response,
              "one FHitInst with the Wolf actor as its target (its cylinder to export, the impact at it); the pawn is none");
        const bootstrap::HitTargetOut& tg = conf->targets[0];
        if ((tg.flags & stats::kHitMiss) == 0) {
            CHECK(tg.mods.size() == 1 && tg.mods[0].stat_mod_guid == hit1->mods[0].stat_mod_guid && tg.mods[0].value < 0 && tg.mods[0].effective < 0 &&
                      std::fabs(tg.mods[0].effective) <= std::fabs(tg.mods[0].value) + 1e-3,
                  "the target's stat mod: the formula's value and the part that took effect (both a loss)");
            CHECKF(world.health_of(wolf) < world.max_health_of(wolf), "the Wolf lost Health (%.1f of %.1f)", world.health_of(wolf), world.max_health_of(wolf));
        } else {
            CHECK(tg.mods.empty(), "a miss has no stat mods");
        }
    }
    // the caster is untouched by the hit: its own Health is full
    const stats::StatValue* hp = rig.cs->find(content::kGuidHealth);
    const stats::StatValue* hp_max = rig.cs->find(content::kGuidMaxHealth);
    CHECK(hp && hp_max && hp->value == hp_max->value, "the caster's Health is untouched");
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");

    // refusals: out of range, dead, an actor that is not there, a friendly ability on an enemy
    const uint64_t far_wolf = w.spawn(wolf_path, 9000, 0, 8, 100);
    TriggerInfo far;
    far.target_count = 1;
    far.target_actors = {far_wolf};
    CHECK(rig.pipe->trigger(ability, 11, t + 3.0, far) == Refusal::OutOfRange, "a Wolf 90 m away is out of range");
    TriggerInfo nobody;
    nobody.target_count = 1;
    CHECK(rig.pipe->trigger(ability, 12, t + 3.1, nobody) == Refusal::None, "a cast naming a target the session could not resolve (no actor) is a cast at the air, as stage 2's");
    // kill the Wolf, then name it
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    world.apply_hit_to_npc(wolf, 77, kill, t + 3.2);
    TriggerInfo dead;
    dead.target_count = 1;
    dead.target_actors = {wolf};
    CHECK(rig.pipe->trigger(ability, 13, t + 3.3, dead) == Refusal::BadTarget, "a dead Wolf is no target");
    TriggerInfo strange;
    strange.target_count = 1;
    strange.target_actors = {123456789};
    CHECK(rig.pipe->trigger(ability, 14, t + 3.4, strange) == Refusal::BadTarget, "an actor that is not in the world is refused");
    // Flourish (a friendly ability: Self only) at an enemy
    const abilities::AbilityDef* flourish = f.catalog->ability_named("Bard_Misc_Flourish");
    if (flourish) {
        TriggerInfo at_enemy;
        at_enemy.target_count = 1;
        at_enemy.target_actors = {far_wolf};
        const Refusal r = rig.pipe->trigger(flourish->row.guid, 15, t + 3.5, at_enemy);
        CHECK(r == Refusal::BadTarget || r == Refusal::OutOfRange, "an ability that only helps allies is not cast at an enemy");
    }
}
// built with C_Y_B and CoA

AOC_REGISTER_TEST(test_player_cast);

// The weapon talents ask GetSkillPurchased about "the tree of the weapon in hand": the Book answers it from the weapon line in hand (held_ranks).  The finisher picks of that tree fire their
// reactions at its finishing hit (the wand's Burning at the beam's end), and not after the weapon in hand changed or without the pick; the damage talents of the tree count for its hits.
void test_weapon_finisher_picks(Fixture& f) {
    std::printf("the weapon talents see the weapon in hand: a finisher pick fires its reaction at that weapon's finishing hit\n");
    Rig rig(4, 25, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    const talents::Tree* wand = rig.talent_catalog->tree_named("Weapon_Ranged_Wand");
    const talents::Tree* axe = rig.talent_catalog->tree_named("Weapon_1H_Axe");
    CHECK(wand && axe && wand->weapon_line != 0 && axe->weapon_line != 0 && wand->weapon_line != axe->weapon_line, "the wand's and the 1H axe's weapon trees, each with a line of its own");
    if (!wand || !axe) return;
    const auto node_of = [](const talents::Tree& tree, const char* skill) -> const talents::Node* {
        for (const talents::Node& n : tree.nodes)
            if (n.skill_name == skill && !n.ranks.empty()) return &n;
        return nullptr;
    };
    const talents::Node* burning = node_of(*wand, "Weapon_Finisher_Burning");
    const talents::Node* bleed = node_of(*axe, "Weapon_Finisher_Bleed");
    CHECK(burning && bleed, "the wand's Burning pick and the axe's Bleed pick");
    if (!burning || !bleed) return;
    const auto marker_holds = [](const std::vector<stats::SkillRank>& held, uint64_t type, int64_t guid) {
        for (const stats::SkillRank& s : held)
            if (s.cls == stats::kWeaponTreeRef && s.skill.type_id == type && s.skill.guid == guid) return true;
        return false;
    };

    // the Book: with the wand's pick bought, the marker holds it while the wand is in hand and only then
    rig.book->load(wand->guid, {burning->ranks.front().guid});
    const auto with_wand = rig.book->held_ranks(wand->weapon_line);
    CHECK(marker_holds(with_wand, stats::kRefTypeSkill, burning->skill) && marker_holds(with_wand, stats::kRefTypeSkillRank, burning->ranks.front().guid),
          "the wand in hand: the Burning pick is held under the weapon-in-hand marker, as its skill and as its rank");
    CHECK(!marker_holds(rig.book->held_ranks(axe->weapon_line), stats::kRefTypeSkill, burning->skill) && !marker_holds(rig.book->held_ranks(0), stats::kRefTypeSkill, burning->skill) &&
              !marker_holds(rig.book->held_ranks(-1), stats::kRefTypeSkill, burning->skill),
          "...not with the axe in hand, with none in hand (0), nor with a weapon line that no weapon tree has (-1)");
    size_t under_marker = 0, in_wand_tree = 0;
    bool in_tree = false;
    for (const stats::SkillRank& s : with_wand) {
        under_marker += s.cls == stats::kWeaponTreeRef ? 1 : 0;
        in_wand_tree += s.cls.type_id == stats::kRefTypeSkillTree && s.cls.guid == wand->guid ? 1 : 0;
        in_tree = in_tree || (s.cls.type_id == stats::kRefTypeSkillTree && s.cls.guid == wand->guid && s.skill.type_id == stats::kRefTypeSkill && s.skill.guid == burning->skill);
    }
    CHECKF(under_marker == in_wand_tree && under_marker > 0, "the marker holds exactly what the wand's tree holds (%zu of %zu)", under_marker, in_wand_tree);
    CHECK(in_tree, "(the pick is still held under its own tree, as before)");

    // the world: a Wolf 6 m away, the wand's and the axe's combos known
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 600, 0, 8, 100);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    rig.know({"WeaponCombo_Ranged_Wand", "WeaponCombo_Melee_1H_Axe"});
    const abilities::AbilityDef* wand_combo = f.catalog->ability_named("WeaponCombo_Ranged_Wand");
    const abilities::AbilityDef* axe_combo = f.catalog->ability_named("WeaponCombo_Melee_1H_Axe");
    const abilities::HitDef* beam_end = f.catalog->hit_named("Weapon_Wand_Beam_ComboEnd_Hit");
    const abilities::HitDef* axe_finisher = f.catalog->hit_named("WeaponCombo_Axe_1H_Hit4+");
    const abilities::HitDef* burning_hit = f.catalog->hit_named("Weapon_Finisher_Burning");
    const abilities::HitDef* bleeding_hit = f.catalog->hit_named("Weapon_Finisher_Bleeding");
    CHECK(wolf != 0 && wand_combo && axe_combo && beam_end && axe_finisher && burning_hit && bleeding_hit, "a Wolf, the two combos, their finishing hits and the two reactions' hits");
    if (!wolf || !wand_combo || !axe_combo || !beam_end || !axe_finisher || !burning_hit || !bleeding_hit) return;
    const auto targeting_of = [](const abilities::AbilityDef& def, const abilities::HitDef& hit) {
        int64_t targeting = content::kTargetingDefaultSelf;
        for (const content::AbilityHitRow& h : def.hits) {
            if (h.hit_guid != hit.row.guid || h.source != "hit") continue;
            for (const content::TargetingRow& tg : def.targetings)
                if (tg.key == h.key) targeting = tg.guid;
        }
        return targeting;
    };

    // what the character holds, as the session hands it over: the ranks with the line in hand, and the effects (carriers) of the ranks of that line's tree
    const auto hold = [&](int64_t ranks_line, int64_t carriers_line) {
        rig.cs->set_passives(rig.book->passives(carriers_line));
        rig.cs->set_ranks(rig.book->held_ranks(ranks_line));
        rig.pipe->set_reaction_carriers(rig.book->reaction_carriers(carriers_line));
        rig.pipe->refresh(rig.clock, "the weapon in hand", true);
        rig.take();
    };
    // One finishing hit of `combo` at the Wolf (the cast, then the client's request for the hit, 0.8 s later as the beam's end comes): how many times the reaction's hit was confirmed, and what the
    // finishing hit did to the Wolf (the formula's value and the roll's flags).
    struct Finish {
        int    reacted = 0;
        double value = 0;
        int    flags = -1;
    };
    double t = 100.0;
    int32_t id = 500;
    const auto finish = [&](const abilities::AbilityDef& combo, int anim, const abilities::HitDef& hit, const abilities::HitDef& reaction_hit, bool check = true) {
        Finish out;
        t += 5.0;
        TriggerInfo info;
        info.animation_index = anim;
        info.target_count = 1;
        info.target_actors = {wolf};
        const Refusal refusal = rig.pipe->trigger(combo.row.guid, id++, t, info);
        if (check) CHECKF(refusal == Refusal::None, "%s is accepted (%d)", combo.row.name.c_str(), static_cast<int>(refusal));
        rig.run_to(t + 0.4);
        const auto verdict = rig.pipe->hit_request(combo.row.guid, targeting_of(combo, hit), hit.row.guid, 1, 0, t + 0.8, {wolf});
        if (check) CHECKF(verdict == AbilityPipeline::HitVerdict::Scheduled, "the request for %s is scheduled", hit.row.name.c_str());
        int landed = 0;
        for (const Seen& s : rig.run_to(t + 1.6)) {
            for (const bootstrap::HitConfirm& c : s.confirmations) {
                if (c.record_guid == hit.row.guid && c.source_type == content::kSourceTypeAbility) {
                    ++landed;
                    if (!c.targets.empty()) {
                        out.flags = c.targets[0].flags;
                        out.value = c.targets[0].mods.empty() ? 0.0 : c.targets[0].mods[0].value;
                    }
                }
                out.reacted += c.record_guid == reaction_hit.row.guid && c.source_type == content::kSourceTypeReaction ? 1 : 0;
            }
        }
        if (check) CHECKF(landed == 1, "%s landed once (%d)", hit.row.name.c_str(), landed);
        return out;
    };

    // the wand in hand with the Burning pick: the reaction fires at every beam's end (no chance, no limit in its data)
    hold(wand->weapon_line, wand->weapon_line);
    const size_t fired0 = rig.pipe->reactions_fired();
    int burns = 0;
    for (int i = 0; i < 3; ++i) burns += finish(*wand_combo, 4, *beam_end, *burning_hit).reacted;
    CHECKF(burns == 0 && rig.pipe->reactions_fired() == fired0 + 3, "the wand in hand with the Burning pick: Weapon_Finisher_Burning fires at each of 3 beam ends; its hit only raises the burn, so none is confirmed (%d confirmed, %zu reactions)", burns, rig.pipe->reactions_fired() - fired0);

    // the weapon in hand changes while the pick's effect is still carried (only the answer to the marker differs): the reaction does not fire
    hold(axe->weapon_line, wand->weapon_line);
    const size_t fired1 = rig.pipe->reactions_fired();
    const int after_swap = finish(*wand_combo, 4, *beam_end, *burning_hit).reacted;
    CHECKF(after_swap == 0 && rig.pipe->reactions_fired() == fired1, "...the same beam end with the axe's line in hand: nothing fires (%d)", after_swap);
    hold(0, wand->weapon_line);
    const size_t fired2 = rig.pipe->reactions_fired();
    const int none_in_hand = finish(*wand_combo, 4, *beam_end, *burning_hit).reacted;
    CHECKF(none_in_hand == 0 && rig.pipe->reactions_fired() == fired2, "...and with no weapon in hand: nothing fires (%d)", none_in_hand);
    hold(wand->weapon_line, wand->weapon_line);
    const size_t fired3 = rig.pipe->reactions_fired();
    CHECK(finish(*wand_combo, 4, *beam_end, *burning_hit).reacted == 0 && rig.pipe->reactions_fired() == fired3 + 1, "...back to the wand: it fires again");

    // the pick gone (refunded): the wand in hand, the same beam end: nothing fires
    rig.book->load(wand->guid, {});
    hold(wand->weapon_line, wand->weapon_line);
    const size_t fired4 = rig.pipe->reactions_fired();
    const int no_pick = finish(*wand_combo, 4, *beam_end, *burning_hit).reacted;
    CHECKF(no_pick == 0 && rig.pipe->reactions_fired() == fired4, "without the pick: nothing fires (%d)", no_pick);

    // another weapon's pick: the axe's Bleed fires its reaction at the 1H axe's proc'd finishing hit, with the axe in hand
    rig.book->load(axe->guid, {bleed->ranks.front().guid});
    hold(axe->weapon_line, axe->weapon_line);
    const size_t fired5 = rig.pipe->reactions_fired();
    const Finish plain = finish(*axe_combo, 0, *axe_finisher, *bleeding_hit);
    const size_t with_pick = rig.pipe->reactions_fired() - fired5;
    CHECKF(plain.reacted == 0 && with_pick >= 1, "the axe in hand with the Bleed pick: Weapon_Finisher_Bleeding fires at the proc'd finishing hit; its hit only applies the bleed, so it is not confirmed (%d confirmed, %zu reactions)", plain.reacted, with_pick);
    hold(wand->weapon_line, axe->weapon_line);
    const size_t fired6 = rig.pipe->reactions_fired();
    const int bleeds_wand = finish(*axe_combo, 0, *axe_finisher, *bleeding_hit).reacted;
    const size_t without = rig.pipe->reactions_fired() - fired6;
    CHECKF(bleeds_wand == 0 && with_pick == without + 1, "...but not with the wand's line in hand (%d confirmed; %zu reactions against %zu: the Fighter's own reactions fire either way)", bleeds_wand, without, with_pick);

    // the damage talents of the tree read the same marker: Damage Focus (+1 % each) and Finisher Damage (+3 % each), three ranks of each, raise the axe's finishing hit by 12 % while the axe is in
    // hand (a hit that was not missed, blocked or critical; the same hit twice, the formula's value is the same but for the talents)
    const auto plain_hit = [&](int64_t ranks_line, int64_t carriers_line) {
        hold(ranks_line, carriers_line);
        Finish r;
        for (int tries = 0; tries < 12; ++tries) {
            r = finish(*axe_combo, 0, *axe_finisher, *bleeding_hit, false);
            if (r.flags == 0 && r.value != 0.0) break;
        }
        CHECKF(r.flags == 0 && r.value != 0.0, "a hit that was not missed, blocked or critical within 12 tries (flags %d)", r.flags);
        return r;
    };
    const Finish base = plain_hit(axe->weapon_line, axe->weapon_line);
    std::vector<int64_t> bought = {bleed->ranks.front().guid};
    for (const char* skill : {"Weapon_Passive_DamageFocus_1", "Weapon_Passive_DamageFocus_2", "Weapon_Passive_DamageFocus_3", "Weapon_Passive_Finisher_Damage_1", "Weapon_Passive_Finisher_Damage_2",
                              "Weapon_Passive_Finisher_Damage_3"}) {
        const talents::Node* n = node_of(*axe, skill);
        if (n) bought.push_back(n->ranks.front().guid);
    }
    CHECKF(bought.size() == 7, "the axe tree has the six damage talents (%zu bought)", bought.size());
    rig.book->load(axe->guid, bought);
    const Finish boosted = plain_hit(axe->weapon_line, axe->weapon_line);
    CHECKF(base.value != 0.0 && boosted.value != 0.0 && near(boosted.value / base.value, 1.12, 1e-3), "the axe's finishing hit with the six damage talents deals 12 %% more (%.3f over %.3f = %.4f)", boosted.value, base.value,
           base.value != 0.0 ? boosted.value / base.value : 0.0);
    const Finish elsewhere = plain_hit(wand->weapon_line, axe->weapon_line);
    CHECKF(elsewhere.value != 0.0 && near(elsewhere.value / base.value, 1.0, 1e-3), "...and the same as without them while the wand's line is in hand (%.4f)", base.value != 0.0 ? elsewhere.value / base.value : 0.0);
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}

AOC_REGISTER_TEST(test_weapon_finisher_picks);

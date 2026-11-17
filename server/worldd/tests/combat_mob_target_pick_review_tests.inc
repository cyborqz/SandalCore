// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/combat_mob_target_pick_review_tests.inc
//
//  Registered tests of test_worldd_combat, the second look at the mobs' choice of a target (combat_mob_target_pick_tests.inc): a move aimed at the player a selector picked hits that
//  player and not the one the creature fights, a move whose selector bounds the distance is started from that distance (a player alone in the band is the case that matters), a
//  step that follows another move is aimed at the player its own selector picks, and a tree's "not the most hated" term picks the player the creature does not fight.  The last test
//  holds the reader and the world to the same text: every selector the reader writes for the game's state trees is a condition the creature table loads.
//  The tables of these tests are written by the test and carry the atoms exactly as the reader writes them (the guids whole, "most" for the hate list's own target).
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

// A move aimed at a picked player: the Giant's BackSwipe (the selector of its tree: behind the creature and within 4.2 m, the blackboard the task reads) with the player it fights
// 3 m in front of it and a second player 2 m behind it.  The cast names the player behind, and its hit finds that player as its target: the one in front is not hit.  (The record
// of the move prefers the creature's active target, and the creature's active target is the one it fights: before, the hit went to the player in front.)
void test_a_move_aimed_at_a_picked_player_hits_that_player_and_not_the_one_it_fights(Fixture& f) {
    std::printf("a BackSwipe aimed at the player behind the Giant hits that player and not the one it fights in front\n");
    const std::string path = creature_path(f, "/Humanoid/Giants/Giant_Base.Default__Giant_Base_C");
    const abilities::AbilityDef* back = f.catalog->ability_named("Giant_BackSwipe");
    PICK_CHECK(!path.empty() && f.motion.find(path) && back, "the Blueprint table has the Giant and the ability data has its BackSwipe");
    if (path.empty() || !f.motion.find(path) || !back) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_aim_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::string pick = "pick(hated,highest,t,bb:ScoredTarget,,face;0.5;neg;lt;d;420;lt;and)";
    const std::vector<std::string> rows = {
        pick_tests::row_with_pick(path, back->row.guid, "BackSwipe", 12, 13, pick, -1, ""),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Giant's table for the aimed BackSwipe reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 300);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Giant's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t giant = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(giant != 0, "the Giant is put in the world");
    if (giant == 0) return;
    Rig rig_b(4, 20, {});
    if (!rig_b.ok()) {
        PICK_CHECK(false, "the second player's rig is made");
        return;
    }
    combat_rig::FakePlayer other(rig_b, 78, {0, 200, 1098});
    field.world.add_player(&other);
    pick_tests::PickLog log;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 30.0 + 1e-9; now += dt) {
        field.world.add_threat(giant, field.player.actor_id(), 200.0, now);
        field.world.add_threat(giant, other.actor_id(), 100.0, now);
        const combat::Vec3 at = pick_tests::place_of(field.w, giant);
        const double yaw = pick_tests::facing_of(field.w, giant) * 3.14159265358979323846 / 180.0;
        field.player.rig_fighter().move_to({at.x + std::cos(yaw) * 300, at.y + std::sin(yaw) * 300, at.z});   // (the one it fights: 3 m in front)
        other.rig_fighter().move_to({at.x - std::cos(yaw) * 200, at.y - std::sin(yaw) * 200, at.z});           // (the one it is aimed at: 2 m behind)
        field.w.npcs->note_player(other.actor_id(), other.rig_fighter().position().x, other.rig_fighter().position().y, now);
        field.tick(now);
        log.look(field.w.registry, giant, now);
    }
    std::printf("  casts: %s; hits on the player behind %zu, on the one in front %zu\n", log.text(*f.catalog).c_str(), other.hits.size(), field.player.hits.size());
    PICK_CHECKF(log.count(back->row.guid) >= 1, "the Giant flanks the player behind it (%zu BackSwipes)", log.count(back->row.guid));
    PICK_CHECKF(log.count(back->row.guid) >= 1 && log.target_of(back->row.guid, 0) == other.actor_id() + 20, "the cast names the player behind it (%llu)",
                static_cast<unsigned long long>(log.target_of(back->row.guid, 0)));
    PICK_CHECKF(!other.hits.empty(), "the BackSwipe hits the player it is aimed at (%zu hits)", other.hits.size());
    PICK_CHECKF(field.player.hits.empty(), "the BackSwipe does not hit the player in front that the Giant fights (%zu hits)", field.player.hits.size());
}

AOC_REGISTER_TEST(test_a_move_aimed_at_a_picked_player_hits_that_player_and_not_the_one_it_fights);

// The case of one player alone: the Tangleweb's selector scores a candidate 5 to 15 m away, so the Pounce is played from there.  With the player 22 m away the spider does not
// pounce; with him 10 m away it does, at once, instead of walking up to bite him first.  (The creature used to start a move only from the reach of its bite, so a player alone in the
// band never saw the Pounce.)
void test_a_webweaver_pounces_at_a_lone_player_in_its_band(Fixture& f) {
    std::printf("a Tangleweb pounces at a lone player 10 m away and not at one 22 m away\n");
    const std::string path = creature_path(f, "/Beast/Arachnid/GiantSpider/Spider_Jungle.Default__Spider_Jungle_C");
    const abilities::AbilityDef* pounce = f.catalog->ability_named("Spider_Pounce");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Spider_Bite");
    PICK_CHECK(!path.empty() && f.motion.find(path) && pounce && bite, "the Blueprint table has the Tangleweb and the ability data has its Pounce and Bite");
    if (path.empty() || !f.motion.find(path) || !pounce || !bite) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_lone_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::string pick = "pick(hated,highest,t,bb:PounceTarget,,d;500;gt;d;1500;lt;and;r;0;if)";
    const std::vector<std::string> rows = {
        pick_tests::row_with_pick(path, pounce->row.guid, "Pounce", 20, 36, pick, 65, "360:0"),
        self_effects::row(path, bite->row.guid, "Bite", 6, 1, 39, -1, 300, 45, "Bite", 0, 1, "", "", "", "360:0", 65),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Tangleweb's table for the lone player reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 2200);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Tangleweb's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t spider = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(spider != 0, "the Tangleweb is put in the world");
    if (spider == 0) return;
    pick_tests::PickLog log;
    const double dt = 1.0 / 15.0, near_from = 10.0;
    for (double now = dt; now <= 30.0 + 1e-9; now += dt) {
        field.world.add_threat(spider, field.player.actor_id(), 200.0, now);
        // the one player holds his place from the spider: 22 m away for 10 s, then 10 m
        const combat::Vec3 at = pick_tests::place_of(field.w, spider);
        field.player.rig_fighter().move_to({at.x, at.y + (now < near_from ? 2200 : 1000), at.z});
        field.tick(now);
        log.look(field.w.registry, spider, now);
    }
    std::printf("  casts: %s\n", log.text(*f.catalog).c_str());
    size_t early = 0;
    for (const pick_tests::PickCast& c : log.casts) early += c.ability == pounce->row.guid && c.t <= near_from ? 1 : 0;
    PICK_CHECKF(early == 0, "no Pounce is played while the only player is 22 m away (%zu casts before %.0f s)", early, near_from);
    PICK_CHECKF(log.first(pounce->row.guid) > near_from, "the Tangleweb pounces once the player is 10 m away (first at %.2f s)", log.first(pounce->row.guid));
    PICK_CHECKF(log.first(pounce->row.guid) > 0 && log.target_of(pounce->row.guid, 0) == field.player.actor_id() + 20, "the Pounce names the lone player (%llu)",
                static_cast<unsigned long long>(log.target_of(pounce->row.guid, 0)));
    PICK_CHECKF(log.first(pounce->row.guid) > 0 && log.first(pounce->row.guid) < near_from + 4.0, "it pounces within 4 s of his coming into the band (at %.2f s)", log.first(pounce->row.guid));
}

AOC_REGISTER_TEST(test_a_webweaver_pounces_at_a_lone_player_in_its_band);

// The tree's "not the most hated" term (the Dunir Mage's Forge Burst, the Rock Elemental's throw): the creature fights the player with the most threat and its move goes to the other one.
// The Pounce is the move here with the selector the reader writes for such a term: 1.0 for any candidate but the most hated, 0.5 for him.
void test_a_selector_that_avoids_the_most_hated_picks_the_other_player(Fixture& f) {
    std::printf("a selector that scores the most hated player lower picks the other player in its band\n");
    const std::string path = creature_path(f, "/Beast/Arachnid/GiantSpider/Spider_Jungle.Default__Spider_Jungle_C");
    const abilities::AbilityDef* pounce = f.catalog->ability_named("Spider_Pounce");
    PICK_CHECK(!path.empty() && f.motion.find(path) && pounce, "the Blueprint table has the Tangleweb and the ability data has its Pounce");
    if (path.empty() || !f.motion.find(path) || !pounce) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_most_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::string pick = "pick(hated,product,t,bb:PounceTarget,d;500;gt;d;1500;lt;and,most;0.5;1;if)";
    const std::vector<std::string> rows = {
        pick_tests::row_with_pick(path, pounce->row.guid, "Pounce", 20, 36, pick, 65, "360:0"),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Tangleweb's table for the most-hated selector reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 800);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Tangleweb's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t spider = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(spider != 0, "the Tangleweb is put in the world");
    if (spider == 0) return;
    Rig rig_b(4, 20, {});
    if (!rig_b.ok()) {
        PICK_CHECK(false, "the second player's rig is made");
        return;
    }
    combat_rig::FakePlayer other(rig_b, 78, {0, 1000, 1098});
    field.world.add_player(&other);
    pick_tests::PickLog log;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 30.0 + 1e-9; now += dt) {
        field.world.add_threat(spider, field.player.actor_id(), 200.0, now);   // (the most hated: the one the spider fights)
        field.world.add_threat(spider, other.actor_id(), 100.0, now);
        const combat::Vec3 at = pick_tests::place_of(field.w, spider);
        field.player.rig_fighter().move_to({at.x, at.y + 800, at.z});    // 8 m: nearer, so the nearest-first order alone would pick him
        other.rig_fighter().move_to({at.x, at.y + 1000, at.z});          // 10 m
        field.w.npcs->note_player(other.actor_id(), other.rig_fighter().position().x, other.rig_fighter().position().y, now);
        field.tick(now);
        log.look(field.w.registry, spider, now);
    }
    std::printf("  casts: %s\n", log.text(*f.catalog).c_str());
    PICK_CHECKF(log.count(pounce->row.guid) >= 1, "the Tangleweb pounces with both players in its band (%zu Pounces)", log.count(pounce->row.guid));
    PICK_CHECKF(log.count(pounce->row.guid) >= 1 && log.target_of(pounce->row.guid, 0) == other.actor_id() + 20, "the Pounce names the player the spider does not fight (%llu)",
                static_cast<unsigned long long>(log.target_of(pounce->row.guid, 0)));
}

AOC_REGISTER_TEST(test_a_selector_that_avoids_the_most_hated_picks_the_other_player);

// A step that follows another move: the Tangleweb's Bite readies the Pounce (the table's `next` link) and the Pounce's selector scores the player 10 m away.  The step is aimed at the
// player its own selector picked, as the tree's first move would be.  (The creature fights the player at its feet; before, the step went to that player.)
void test_a_chained_move_is_aimed_at_the_player_its_selector_picks(Fixture& f) {
    std::printf("a Pounce that follows the Tangleweb's Bite is aimed at the player its selector picks 10 m away, not at the one at its feet\n");
    const std::string path = creature_path(f, "/Beast/Arachnid/GiantSpider/Spider_Jungle.Default__Spider_Jungle_C");
    const abilities::AbilityDef* pounce = f.catalog->ability_named("Spider_Pounce");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Spider_Bite");
    PICK_CHECK(!path.empty() && f.motion.find(path) && pounce && bite, "the Blueprint table has the Tangleweb and the ability data has its Pounce and Bite");
    if (path.empty() || !f.motion.find(path) || !pounce || !bite) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_chain_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    const std::string pick = "pick(hated,highest,t,bb:PounceTarget,,d;500;gt;d;1500;lt;and;r;0;if)";
    // the Bite names the Pounce in its `next` column; the Pounce is a later task of the tree (order 40) that the Bite's end readies
    const std::string bite_row = self_effects::row(path, bite->row.guid, "Bite", 6, 1, 39, -1, 300, -1, "Bite", 0, 1, "", "", "", "360:0", 65) + "\t" + std::to_string(pounce->row.guid);
    const std::string pounce_row = pick_tests::row_with_pick(path, pounce->row.guid, "Pounce", 20, 40, pick, 65, "360:0") + "\t";
    {
        std::ofstream o((tmp / "abilities.tsv").string(), std::ios::binary);
        o << self_effects::kHeader << "\tnext\n" << bite_row << "\n" << pounce_row << "\n";
    }
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    combat::NpcAbilityData tables;
    std::string err;
    const bool loaded = tables.load((tmp / "abilities.tsv").string(), dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
    PICK_CHECKF(loaded, "the Tangleweb's table with the Bite's next link reads (%s)", loaded ? "yes" : err.c_str());
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 300);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Tangleweb's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t spider = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(spider != 0, "the Tangleweb is put in the world");
    if (spider == 0) return;
    Rig rig_b(4, 20, {});
    if (!rig_b.ok()) {
        PICK_CHECK(false, "the second player's rig is made");
        return;
    }
    combat_rig::FakePlayer other(rig_b, 78, {0, 1000, 1098});
    field.world.add_player(&other);
    pick_tests::PickLog log;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        field.world.add_threat(spider, field.player.actor_id(), 200.0, now);   // (the one it fights: at its feet)
        field.world.add_threat(spider, other.actor_id(), 100.0, now);
        const combat::Vec3 at = pick_tests::place_of(field.w, spider);
        field.player.rig_fighter().move_to({at.x, at.y + 300, at.z});
        other.rig_fighter().move_to({at.x, at.y + 1000, at.z});
        field.w.npcs->note_player(other.actor_id(), other.rig_fighter().position().x, other.rig_fighter().position().y, now);
        field.tick(now);
        log.look(field.w.registry, spider, now);
    }
    std::printf("  casts: %s\n", log.text(*f.catalog).c_str());
    PICK_CHECKF(log.count(bite->row.guid) >= 1 && log.count(pounce->row.guid) >= 1, "the Tangleweb bites and then pounces (%zu Bites, %zu Pounces)", log.count(bite->row.guid), log.count(pounce->row.guid));
    PICK_CHECKF(log.first(pounce->row.guid) > log.first(bite->row.guid) && log.first(bite->row.guid) > 0, "the Pounce comes after the first Bite (Bite at %.2f s, Pounce at %.2f s)", log.first(bite->row.guid),
                log.first(pounce->row.guid));
    PICK_CHECKF(log.count(pounce->row.guid) >= 1 && log.target_of(pounce->row.guid, 0) == other.actor_id() + 20, "the Pounce that follows the Bite names the player its selector picks (%llu)",
                static_cast<unsigned long long>(log.target_of(pounce->row.guid, 0)));
}

AOC_REGISTER_TEST(test_a_chained_move_is_aimed_at_the_player_its_selector_picks);

// The reader and the world agree on the selector's text: every selector the game's state trees hold, as the reader writes it (the 63 different forms of the 89 selectors the world can
// read), is a condition the creature table loads; the effect and alignment records inside them come through whole (a guid is a 63-bit number: written as a rounded number it would not
// read, and the world refuses it instead of reading another record), and a selector that names the hate list's own target reads as `most`.
namespace pick_tests {

/// Whether `term` or an operand of it is of `kind` and names `guid`.
bool names_record(const combat::PickTerm& term, combat::PickTerm::Kind kind, int64_t guid) {
    if (term.kind == kind && term.effect == guid) return true;
    for (const combat::PickTerm& a : term.args) {
        if (names_record(a, kind, guid)) return true;
    }
    return false;
}

bool rule_names_record(const combat::PickRule& rule, combat::PickTerm::Kind kind, int64_t guid) {
    for (const combat::PickTerm& t : rule.filters) if (names_record(t, kind, guid)) return true;
    for (const combat::PickTerm& t : rule.considerations) if (names_record(t, kind, guid)) return true;
    return false;
}

bool rule_has(const combat::PickRule& rule, combat::PickTerm::Kind kind) {
    const std::function<bool(const combat::PickTerm&)> has = [&](const combat::PickTerm& t) {
        if (t.kind == kind) return true;
        for (const combat::PickTerm& a : t.args) if (has(a)) return true;
        return false;
    };
    for (const combat::PickTerm& t : rule.filters) if (has(t)) return true;
    for (const combat::PickTerm& t : rule.considerations) if (has(t)) return true;
    return false;
}

/// Loads a table of one Blueprint with one Pounce row per condition text and returns whether it reads.
bool table_of_conditions_reads(const std::string& path, int64_t ability, const std::vector<std::string>& conditions, combat::NpcAbilityData& tables) {
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_atoms_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    std::vector<std::string> rows;
    int order = 1;
    for (const std::string& c : conditions) rows.push_back(self_effects::row(path, ability, "Pounce", 20, 1, order++, -1, -1, -1, "Pounce", 0, 1, "c0:" + c, "", "", "360:0", 65));
    std::ofstream o((tmp / "abilities.tsv").string(), std::ios::binary);
    o << self_effects::kHeader << "\n";
    for (const std::string& r : rows) o << r << "\n";
    o.close();
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string err;
    const bool loaded = tables.load((tmp / "abilities.tsv").string(), dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
    fs::remove_all(tmp, ec);
    return loaded;
}

}  // namespace pick_tests

void test_the_selectors_the_reader_writes_all_read(Fixture& f) {
    std::printf("the creature table reads every selector atom the reader writes for the game's state trees, with the records whole and the hate list's own target as `most`\n");
    const std::string path = creature_path(f, "/Beast/Arachnid/GiantSpider/Spider_Jungle.Default__Spider_Jungle_C");
    const abilities::AbilityDef* pounce = f.catalog->ability_named("Spider_Pounce");
    PICK_CHECK(!path.empty() && pounce, "the Blueprint table has the Tangleweb and the ability data has its Pounce");
    if (path.empty() || !pounce) return;
    static const char* const kAtoms[] = {
        "pick(hated,highest,g,bb:ScoredTarget,,face;0.5;neg;lt;d;420;lt;and)",
        "pick(hated,highest,g,bb:ScoredTarget,,ang;90;gt;d;520;lt;0.2;1;rr;0;if;0;if)",
        "pick(hated,product,g,bb:DescVoidTarget,d;1500;lt,r;0.01;+)",
        "pick(hated,product,g,bb:DestrTarget,d;1200;lt)",
        "pick(hated,product,g,,d;1200;lt)",
        "pick(perceived,highest,g,,friendly,self;not;d;1000;lt;and;h;0;gt;and)",
        "pick(hated,product,g,,d;1200;lt,r;0.01;+)",
        "pick(hated,product,g,!c;bb:Target,d;3500;lt;seff:6064633776496510988;0;eq;d;2250;lt;or;and,eff:6064631979899944981;0;gt;0.9;0.5;if;0.1;neg;0.1;rr;+)",
        "pick(perceived,product,g,c5;bb:Heal%20Target,friendly;d;2500;lt;and;h;0.6;lt;and;srank;rank;-;1;le;and,1;h;-)",
        "pick(perceived,product,g,s;bb:SerenadeTarget,friendly;h;0;gt;and;self;not;and)",
        "pick(hated,product,g,bb:Target,d;350;lt,r)",
        "pick(hated,product,g,bb:Volley%20Target,d;3500;lt;d;1200;gt;and,d;2300;-;abs;1200;/)",
        "pick(hated,product,g,bb:Target,d;2000;lt,0.05;1;1;d;1000;-;abs;1000;/;-;lerp)",
        "pick(perceived,product,g,bb:Target,friendly;eff:6064632636917022720;0;eq;and;d;2500;lt;and,self;0.5;eff:6064632638083173063;0;gt;0.1;1;if;if)",
        "pick(perceived,product,g,bb:Target,friendly;self;not;and;d;2500;lt;and;h;0.7;lt;and,1;h;-)",
        "pick(hated,product,g,bb:ForgeBurstTarget,,most;0.01;1;if,d;1900;gt;los;not;or;0;1;if)",
        "pick(hated,product,g,bb:LightningOrbTarget,,300;near;0;gt;2;1;if,d;1900;gt;los;not;or;0;1;if)",
        "pick(hated,product,g,bb:RockThrowTarget,d;1200;lt,d;500;gt;most;0.5;1;if;2.5;*;most;0.5;1;if;if)",
        "pick(hated,product,g,bb:EarthSplitterTarget,d;1500;lt;d;450;gt;and,most;1;0.5;if,d;500;/;0;1;clamp)",
        "pick(hated,product,g,bb:Target,d;2000;lt;d;800;gt;and,0.05;1;1;d;1000;-;abs;1000;/;-;lerp)",
        "pick(perceived,product,g,bb:Target,friendly;eff:6064633611559502138;0;eq;and;eff:6064633612333875299;0;gt;and;d;2500;lt;and,self;0.5;eff:6064632638083173063;0;gt;0.1;1;if;if)",
        "pick(perceived,product,g,bb:Target,friendly;eff:6064633612333875299;0;gt;and;self;not;and;d;2500;lt;and;h;0.7;lt;and,1;h;-)",
        "pick(hated,product,g,bb:ForgeBurstTarget,,most;0.01;1;if,d;1500;gt;0;1;if)",
        "pick(hated,product,g,bb:ChainEnfeebleTarget,d;1500;lt,d;0.1;*)",
        "pick(hated,product,g,bb:LightningOrbTarget,,300;near;0;gt;2;1;if,d;1500;gt;0;1;if)",
        "pick(hated,product,g,bb:Target,d;1800;lt;los;and)",
        "pick(hated,highest,g,bb:StoredTarget,,face;0.5;neg;lt;d;320;lt;and)",
        "pick(hated,product,g,bb:ForgeBurstTarget,,most;0.01;1;if)",
        "pick(hated,product,g,bb:Shot%20Target,d;3000;lt,h)",
        "pick(hated,product,g,bb:Spine%20Target,d;3000;lt,eff:6064631828041695232;0;gt;h;0.1;*;h;if)",
        "pick(hated,highest,g,bb:ChargeTarget,,d;3600;lt;d;1000;gt;and;0.6;1;rr;d;3600;lt;0.5;0.2;rr;0.1;if;if)",
        "pick(hated,highest,g,bb:FixateTarget,,0.1;1;rr)",
        "pick(hated,highest,g,bb:ChargeTarget,,d;2400;lt;d;1000;gt;and;0.6;1;rr;d;2400;lt;0.5;0.2;rr;0.1;if;if)",
        "pick(hated,highest,g,,,d;2000;lt)",
        "pick(hated,highest,g,bb:PounceTarget,,d;500;gt;d;2000;lt;and;r;0;if)",
        "pick(hated,product,g,bb:Shard%20Target,d;3500;lt,r)",
        "pick(perceived,product,g,bb:Spray%20Target,friendly;self;not;and;d;1500;lt;and;h;0.8;lt;and,1;d;1500;/;-)",
        "pick(hated,product,g,bb:Target,d;3000;lt,r,d;800;lt;d;800;/;1;if)",
        "pick(hated,product,g,bb:Whirlwind%20Target,d;400;lt,400;d;-;400;/)",
        "pick(perceived,product,g,bb:Shell%20Target,friendly;d;2500;lt;and;eff:6064631436748718277;0;gt;and;friendly;self;or;and;eff:6064633815352803328;0;eq;and;eff:6064630129397465246;0;eq;and;eff:6064631596713705509;0;eq;and,1;h;-;0.1;max,self;1;0.5;if)",
        "pick(hated,product,g,bb:RandomTarget,,r)",
        "pick(hated,product,g,bb:RandomTarget,,r,d;750;lt,los)",
        "pick(hated,product,g,bb:RandomTarget,,r,d;2500;lt,ang;10;lt,los)",
        "pick(hated,product,g,bb:RandomTarget,,r,d;750;lt)",
        "pick(hated,product,g,bb:RandomTarget,,r,d;2500;lt,ang;10;lt)",
        "pick(perceived,product,g,c4;bb:MendTarget,friendly;d;2500;lt;and;h;0.8;lt;and;eff:6064633417608134662;1;lt;and,self;not;bb:Heal%20Target;and;1;self;1;h;-;1;h;-;*;1;h;-;if;if)",
        "pick(hated,product,g,bb:Target,d;1800;lt;los;and,r)",
        "pick(hated,product,g,bb:Target,d;3500;lt,d;1200;gt;r;0.9;*;0.1;+;0.1;if)",
        "pick(hated,product,g,bb:Target,,d;3500;lt;d;1500;gt;and,eff:6064631979818156032;0;gt;1;0.5;if,r)",
        "pick(perceived,product,g,c4;bb:SoultheftTarget,eff:6064629309380689954;0;gt;eff:6064631477873935123;0;le;and,1;d;/)",
        "pick(hated,product,g,bb:SprayTarget,d;1400;lt,most;0.1;1;if,d;1000;/)",
        "pick(hated,product,g,,eff:6064633140498989056;0;le,d;1000;/)",
        "pick(hated,highest,g,bb:PounceTarget,,d;500;gt;d;1500;lt;and;r;0;if)",
        "pick(hated,product,g,bb:Target,d;1500;lt,d;1500;/)",
        "pick(hated,product,g,bb:Target,d;300;lt;ang;120;gt;and,r)",
        "pick(hated,product,g,bb:ChargeTarget,d;600;gt,r;0.1;+)",
        "pick(hated,product,g,bb:ChargeTarget,,r;0.1;+,d)",
        "pick(hated,product,g,bb:RushTarget,d;500;gt,r)",
        "pick(hated,product,g,bb:LungeTarget,d;800;lt,most;0.5;1;if)",
        "pick(hated,product,g,bb:LungeTarget,eff:6064633695552995335;0;gt,800;d;/)",
        "pick(perceived,product,g,bb:Possess%20Target,friendly;align:6064633729734213635;not;and;align:6064631640447713974;not;and;srank;rank;le;and;d;2500;lt;and,1)",
        "pick(hated,product,g,bb:Target,d;1600;lt;los;and,0.1;1;d;1000;-;abs;1000;/;-;max)",
        "pick(hated,product,g,bb:Vomit%20Target,d;2500;lt;d;1000;gt;and;angc;90;lt;and;los;and,1)"};
    std::vector<std::string> atoms(std::begin(kAtoms), std::end(kAtoms));
    combat::NpcAbilityData tables;
    const bool loaded = pick_tests::table_of_conditions_reads(path, pounce->row.guid, atoms, tables);
    PICK_CHECKF(loaded, "a table with all %zu selector atoms of the reader loads", atoms.size());
    if (!loaded) return;
    const std::vector<combat::CreatureAbility>* tasks = tables.abilities_of(path);
    PICK_CHECKF(tasks && tasks->size() == atoms.size(), "every row is kept (%zu of %zu)", tasks ? tasks->size() : 0, atoms.size());
    if (!tasks) return;
    size_t picks = 0;
    bool mend_guid = false, arsonist_guid = false, shell_guid = false, wraith_alignment = false, most = false, bb_name = false;
    for (const combat::CreatureAbility& t : *tasks) {
        for (const combat::CondGroup& g : t.groups) {
            for (const combat::CondToken& token : g) {
                if (token.atom.kind != combat::CondAtom::Kind::Pick) continue;
                ++picks;
                const combat::PickRule& r = token.atom.pick;
                mend_guid = mend_guid || pick_tests::rule_names_record(r, combat::PickTerm::Kind::HasEffect, 6064633417608134662ll);
                arsonist_guid = arsonist_guid || pick_tests::rule_names_record(r, combat::PickTerm::Kind::SourceHasEffect, 6064633776496510988ll);
                shell_guid = shell_guid || pick_tests::rule_names_record(r, combat::PickTerm::Kind::HasEffect, 6064631436748718277ll);
                wraith_alignment = wraith_alignment || pick_tests::rule_names_record(r, combat::PickTerm::Kind::Alignment, 6064633729734213635ll);
                most = most || pick_tests::rule_has(r, combat::PickTerm::Kind::IsMostHated);
                bb_name = bb_name || r.bb == "Heal Target" || r.bb == "Shot Target";
            }
        }
    }
    PICK_CHECKF(picks == atoms.size(), "every row is a selector condition (%zu of %zu)", picks, atoms.size());
    PICK_CHECK(mend_guid, "the Mender's healing effect is in its selector with its whole guid");
    PICK_CHECK(arsonist_guid, "the Arsonist's effect on itself is in its selector with its whole guid");
    PICK_CHECK(shell_guid, "the Streamweaver's effect is in its selector with its whole guid");
    PICK_CHECK(wraith_alignment, "the Wraith's alignment is in its selector with its whole guid");
    PICK_CHECK(most, "a selector reads the hate list's own target as `most`");
    PICK_CHECK(bb_name, "a blackboard variable with a space in its name is read back with the space");
    // a guid the reader wrote as a rounded number (6.06E+18) is not a record: the table does not read, it never reads another record
    combat::NpcAbilityData rounded;
    PICK_CHECK(!pick_tests::table_of_conditions_reads(path, pounce->row.guid, {"pick(hated,product,g,,eff:6.064633417608135E+18;0;gt)"}, rounded), "a record written as a rounded number is refused, not read as another record");
}

AOC_REGISTER_TEST(test_the_selectors_the_reader_writes_all_read);

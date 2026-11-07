// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_humanoid_tests.inc
//
//  Registered tests of test_worldd_combat: the humanoid creatures against a standing player: the bandit raider, the arsonist, the goblin shaman, flayer and languid goblin, and the walk up to a far player.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

void test_bandit_raider_fights_a_standing_player(Fixture& f) {
    std::printf("a bandit raider against a player who stands still: made from a guard's open with its tree's abilities, it strikes within reach, lands each hit at its montage's notify, "
                "holds its specials back by the tree's timers and does not drift\n");
    constexpr double kDt = 1.0 / 15.0, kSeconds = 60.0;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    struct Kind {
        const char* leaf;
        bool        specials;   // its tree switches the cleave and the whirlwind on
    };
    for (const Kind kind : {Kind{"/Bandit_Raider/CSB_Bandit_Raider_StateTree.Default__CSB_Bandit_Raider_StateTree_C", true}, Kind{"/Bandit_Raider/Bandit_Raider.Default__Bandit_Raider_C", false}}) {
        const std::string path = creature_path(f, kind.leaf);
        const auto bp = f.motion.find(path);
        const std::string name = spawns::blueprint_leaf(path);
        CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the %s and the creature tables list its abilities", name.c_str());
        if (path.empty() || !bp || !tables.abilities_of(path)) return;
        CHECKF(spawns::fighting_humanoid(path), "%s is a hostile humanoid whose fights are built", name.c_str());
        BanditOpen made;
        CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the %s (verdict: %s)", name.c_str(), spawns::verdict_name(made.why));
        if (!made.open) return;
        // its open lists every ability of its tree (the client plays only the abilities it has the record of)
        {
            combat_rig::World probe;
            if (!probe.problem.empty()) return;
            const uint64_t id = probe.spawn(path, 0, 0, 7, 900, bp->half_height, made.open);
            const world::WorldActor* a = probe.registry.find(id);
            size_t listed = 0, wanted = 0;
            std::set<int64_t> seen;
            for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
                if (!seen.insert(t.ability).second) continue;
                ++wanted;
                if (a && combat::npc_knows_ability(*a, t.ability)) ++listed;
            }
            CHECKF(a && wanted >= 5 && listed == wanted, "the %s's open lists the %zu abilities of its tree (%zu listed)", name.c_str(), wanted, listed);
            CHECKF(a && a->npc.collision_cylinder != 0, "a hit on the %s is confirmed against its collision cylinder, which its open names", name.c_str());
        }
        std::vector<LungeSample> samples;
        std::vector<combat::IncomingHit> hits;
        combat::CombatWorld::Stats stats;
        CHECKF(run_lunge_fight(f, tables, path, kSeconds, 0, 800, nullptr, samples, &hits, false, -1, &stats, 40000.0, made.open), "a fight of %.0f s against a player who stands 8 m away runs", kSeconds);
        if (samples.empty()) return;
        check_targets_named_in_full(samples, name.c_str());

        // the casts as the client sees them: the start of each montage element, its ability and its montage
        struct Cast {
            double      t;
            int64_t     ability;
            uint64_t    montage;
            std::string label;
        };
        std::vector<Cast> casts;
        double chase_began = -1;
        for (const LungeSample& s : samples) {
            if (chase_began < 0 && std::hypot(s.wx, s.wy) > 1.0) chase_began = s.t;
            if (!s.cast_began) continue;
            const abilities::AbilityDef* def = f.catalog->ability(s.ability);
            casts.push_back({s.t, s.ability, s.montage, def ? def->row.name : std::string("?")});
        }
        std::map<std::string, std::vector<double>> by_name;
        for (const Cast& c : casts) by_name[c.label].push_back(c.t);
        std::string summary;
        for (const auto& [label, times] : by_name) summary += (summary.empty() ? "" : ", ") + label + " " + std::to_string(times.size());
        std::printf("  %s, %.0f s: %zu casts (%s); the chase began at %.2f s\n", name.c_str(), kSeconds, casts.size(), summary.c_str(), chase_began);
        {
            std::string line;   // the first casts in order, for reading the fight
            for (size_t i = 0; i < casts.size() && i < 24; ++i) {
                std::string label = casts[i].label;
                if (label.rfind("Bandit_Raider_", 0) == 0) label.erase(0, 14);
                line += (line.empty() ? "" : " ") + label + "@" + std::to_string(casts[i].t - std::max(0.0, chase_began)).substr(0, 5);
            }
            std::printf("  casts, seconds after the chase began: %s\n", line.c_str());
            std::vector<double> gaps;   // from the start of one strike to the start of the next one (nothing between)
            for (size_t i = 1; i < casts.size(); ++i) {
                if (casts[i].label == "Bandit_Raider_Strike" && casts[i - 1].label == "Bandit_Raider_Strike") gaps.push_back(casts[i].t - casts[i - 1].t);
            }
            std::sort(gaps.begin(), gaps.end());
            if (!gaps.empty()) std::printf("  strike to strike: %zu gaps, %.2f s at least, %.2f s in the middle, %.2f s at most\n", gaps.size(), gaps.front(), gaps[gaps.size() / 2], gaps.back());
            // a strike is over at its montage's animation-end notify (1.0 s in) and the 1 s global cooldown runs from there: 2.0 s from strike to strike, plus the 0.2 s the creature looks at
            CHECKF(gaps.size() >= 5 && gaps.front() >= 2.0 - 1e-6 && gaps.back() <= 2.4, "strikes come 2.0 to 2.4 s apart, not at the montage's length (%.2f to %.2f s)", gaps.empty() ? 0.0 : gaps.front(), gaps.empty() ? 0.0 : gaps.back());
        }
        const auto count = [&](const char* label) { return by_name.count(label) ? by_name[label].size() : size_t{0}; };
        CHECKF(casts.size() >= 12, "it plays its own animations (%zu casts in %.0f s; with the single swing it would play none)", casts.size(), kSeconds);
        CHECKF(count("Bandit_Raider_Strike") >= 8, "it strikes over and over (%zu strikes)", count("Bandit_Raider_Strike"));
        CHECKF(count("Bandit_Raider_HeavySlash") + count("Bandit_Raider_Whirlwind") >= 1, "it uses a special attack once the tree's timer for it has run (%zu heavy slashes, %zu whirlwinds)", count("Bandit_Raider_HeavySlash"),
               count("Bandit_Raider_Whirlwind"));
        if (kind.specials) {
            // the cleave waits for the fight's 8th second, as the tree's Float Compare says (Combat Timer > 8)
            const std::vector<double>& cleaves = by_name["Bandit_Raider_BrutalCleave"];
            CHECKF(cleaves.size() >= 3, "it cleaves several times in %.0f s (%zu)", kSeconds, cleaves.size());
            if (!cleaves.empty()) {
                std::printf("  the first cleave came %.2f s into the chase; the cleaves came %.2f s apart on average\n", cleaves.front() - chase_began,
                            cleaves.size() > 1 ? (cleaves.back() - cleaves.front()) / static_cast<double>(cleaves.size() - 1) : 0.0);
                CHECKF(cleaves.front() - chase_began >= 8.0 - 2 * kDt && cleaves.front() - chase_began <= 14.0, "the first cleave comes after the fight's 8th second (%.2f s into the chase)", cleaves.front() - chase_began);
                for (size_t i = 1; i < cleaves.size(); ++i) {
                    CHECKF(cleaves[i] - cleaves[i - 1] >= 8.0, "the cleave's own cooldown of 8 s holds (%.2f s between two)", cleaves[i] - cleaves[i - 1]);
                }
            }
        } else {
            CHECKF(count("Bandit_Raider_BrutalCleave") == 0 && count("Bandit_Raider_Whirlwind") == 0, "its tree switches the cleave and the whirlwind off (%zu, %zu)", count("Bandit_Raider_BrutalCleave"),
                   count("Bandit_Raider_Whirlwind"));
        }
        // the special attacks wait for the tree's number rolled when the fight began (35 % none, else 6 to 9 s), and share a cooldown of 8 s
        std::vector<double> specials;
        for (const Cast& c : casts) {
            if (c.label == "Bandit_Raider_HeavySlash" || c.label == "Bandit_Raider_Whirlwind") specials.push_back(c.t);
        }
        for (size_t i = 1; i < specials.size(); ++i) CHECKF(specials[i] - specials[i - 1] >= 8.0, "two special attacks are at least the shared cooldown of 8 s apart (%.2f s)", specials[i] - specials[i - 1]);

        // every hit of a cast lands at its montage's notify, once, and no hit lands outside a cast
        size_t due = 0, landed = 0, stray = 0;
        double worst_delay = 0;
        std::vector<bool> used(hits.size(), false);
        for (const Cast& c : casts) {
            const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
            if (!md) continue;
            const std::vector<double> bites = cast_bites(f, tables, c.ability, c.montage);
            if (c.t + md->seconds(1.0) > kSeconds - 0.3) continue;   // (a cast the fight ended in)
            for (const double at : bites) {
                ++due;
                bool found = false;
                for (size_t i = 0; i < hits.size() && !found; ++i) {
                    if (used[i] || hits[i].source_guid != c.ability) continue;
                    const double delay = hits[i].at - (c.t + at);
                    if (delay < -1e-6 || delay > 2 * kDt + 1e-6) continue;   // (the hit is delivered at the first tick on or after its notify)
                    used[i] = true;
                    found = true;
                    ++landed;
                    worst_delay = std::max(worst_delay, delay);
                }
            }
        }
        for (size_t i = 0; i < hits.size(); ++i) {
            if (used[i]) continue;
            bool in_cast = false;
            for (const Cast& c : casts) {
                const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
                if (md && hits[i].source_guid == c.ability && hits[i].at >= c.t - 1e-6 && hits[i].at <= c.t + md->seconds(1.0) + 2 * kDt) in_cast = true;
            }
            if (!in_cast) ++stray;
        }
        std::printf("  %zu hits were due at their notifies, %zu landed (the latest %.3f s after its notify); %zu hits outside a cast\n", due, landed, worst_delay, stray);
        CHECKF(due >= 8 && landed == due, "every hit of every cast lands at its montage's notify (%zu of %zu)", landed, due);
        CHECKF(stray == 0, "no hit lands outside a cast (%zu)", stray);

        // in reach and no drift: it starts its attacks within the tree's reach of the player's surface, and once at him it stays about him
        const double r_creature = bp->radius, r_player = 34.0;
        double reach = 0;
        for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
            if (!t.unknown && !t.off && t.fight && t.min_distance <= 0 && t.max_distance > reach) reach = t.max_distance;
        }
        double travel = 0;   // the farthest any readable montage of the tree carries it
        for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
            const abilities::AbilityDef* def = f.catalog->ability(t.ability);
            if (!def) continue;
            for (const content::MontageRow& m : def->montages) {
                const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
                if (md && !md->root_unreadable && !md->root_motion.empty()) travel = std::max(travel, std::hypot(md->root_motion.back().x, md->root_motion.back().y));
            }
        }
        double worst_start = 0, worst_gap = 0;
        size_t drift_ticks = 0;
        for (const LungeSample& s : samples) {
            const double gap = std::hypot(s.wx - s.px, s.wy - s.py) - r_creature - r_player;
            if (s.cast_began) worst_start = std::max(worst_start, gap);
            if (chase_began >= 0 && s.t > chase_began + 3.0) {
                worst_gap = std::max(worst_gap, gap);
                if (gap > reach + travel + 60.0) ++drift_ticks;
            }
        }
        std::printf("  reach %.0f cm, the farthest travel of a montage %.0f cm; the surface gap was at most %.0f cm when a cast began and %.0f cm at any time after the chase\n", reach, travel, worst_start, worst_gap);
        CHECKF(worst_start <= reach + 5.0, "it begins its casts within the tree's reach (%.0f of %.0f cm)", worst_start, reach);
        CHECKF(drift_ticks == 0, "it never drifts away from a player who stands still (%zu ticks farther than reach + travel)", drift_ticks);
        // it ends the fight where it started it: standing about the player, not walking off
        const LungeSample& last = samples.back();
        const double end_gap = std::hypot(last.wx - last.px, last.wy - last.py) - r_creature - r_player;
        CHECKF(end_gap <= reach + travel + 60.0, "at the end of the fight it is still about the player (%.0f cm from his surface)", end_gap);
    }
}

AOC_REGISTER_TEST(test_bandit_raider_fights_a_standing_player);

void test_arsonist_fires_at_a_standing_player(Fixture& f) {
    std::printf("a bandit arsonist against a player who stands 20 m away: it casts its fire bolt about every 2 s and its combo now and then, each bolt leaves at its montage's notify and lands "
                "when it arrives, no hit comes without a bolt, and its spray, stream and ring of fire stay off\n");
    constexpr double kSeconds = 60.0;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    struct Kind {
        const char* leaf;
        bool        combo;   // its tree switches the fire bolt combo on
    };
    for (const Kind kind : {Kind{"/Bandit_Arsonist/Bandit_Arsonist.Default__Bandit_Arsonist_C", false}, Kind{"/Bandit_Arsonist/CSB_Arsonist_StateTree.Default__CSB_Arsonist_StateTree_C", false},
                            Kind{"/Bandit_Arsonist/Bandit_Arsonist_Blister.Default__Bandit_Arsonist_Blister_C", true}}) {
        const std::string path = creature_path(f, kind.leaf);
        const auto bp = f.motion.find(path);
        const std::string name = spawns::blueprint_leaf(path);
        CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the %s and the creature tables list its abilities", name.c_str());
        if (path.empty() || !bp || !tables.abilities_of(path)) return;
        CHECKF(spawns::fighting_humanoid(path), "%s is a hostile humanoid whose fights are built", name.c_str());
        BanditOpen made;
        CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the %s (verdict: %s)", name.c_str(), spawns::verdict_name(made.why));
        if (!made.open) return;
        {   // its open lists every ability of its tree (the client plays only the abilities it has the record of)
            combat_rig::World probe;
            if (!probe.problem.empty()) return;
            const uint64_t id = probe.spawn(path, 0, 0, 7, 900, bp->half_height, made.open);
            const world::WorldActor* a = probe.registry.find(id);
            size_t listed = 0, wanted = 0;
            std::set<int64_t> seen;
            for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
                if (!seen.insert(t.ability).second) continue;
                ++wanted;
                if (a && combat::npc_knows_ability(*a, t.ability)) ++listed;
            }
            CHECKF(a && wanted >= 5 && listed == wanted, "the %s's open lists the %zu abilities of its tree (%zu listed)", name.c_str(), wanted, listed);
        }
        // the tree: the bolt and the combo are usable, the spray, the stream and the ring of fire are left out
        {
            std::map<std::string, std::pair<bool, std::string>> by_name;   // ability -> (usable, why)
            for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
                const abilities::AbilityDef* def = f.catalog->ability(t.ability);
                if (def && t.fight) by_name[def->row.name] = {!t.unknown && !t.off, t.why};
            }
            CHECKF(by_name["Bandit_Arsonist_Firebolt"].first, "the %s's tree can use the fire bolt", name.c_str());
            CHECKF(by_name["Bandit_Arsonist_FireboltCombo"].first == kind.combo, "the %s's tree %s the fire bolt combo (%s)", name.c_str(), kind.combo ? "switches on" : "switches off",
                   by_name["Bandit_Arsonist_FireboltCombo"].second.c_str());
            for (const char* waits : {"Bandit_Arsonist_FlameBurst", "Bandit_Arsonist_FlameThrow", "Bandit_Arsonist_FlameRing"}) {
                CHECKF(by_name.count(waits) && !by_name[waits].first && by_name[waits].second.find("waits for") != std::string::npos, "%s stays off in the %s's tree (%s)", waits, name.c_str(), by_name[waits].second.c_str());
            }
        }
        std::vector<LungeSample> samples;
        std::vector<combat::IncomingHit> hits;
        combat::CombatWorld::Stats stats;
        CHECKF(run_lunge_fight(f, tables, path, kSeconds, 0, 2000, nullptr, samples, &hits, false, 0.5, &stats, 40000.0, made.open), "a fight of %.0f s against a player who stands 20 m away runs", kSeconds);
        if (samples.empty()) return;
        check_targets_named_in_full(samples, name.c_str());
        const std::vector<ShotCast> casts = shot_casts(f, samples);
        std::map<std::string, std::vector<double>> by_name;
        for (const ShotCast& c : casts) by_name[c.name].push_back(c.t);
        std::string summary;
        for (const auto& [label, times] : by_name) summary += (summary.empty() ? "" : ", ") + label + " " + std::to_string(times.size());
        std::printf("  %s, %.0f s: %zu casts (%s), %zu hits\n", name.c_str(), kSeconds, casts.size(), summary.c_str(), hits.size());
        const auto count = [&](const char* label) { return by_name.count(label) ? by_name[label].size() : size_t{0}; };
        CHECKF(count("Bandit_Arsonist_Firebolt") >= 12, "it casts its fire bolt over and over (%zu)", count("Bandit_Arsonist_Firebolt"));
        if (kind.combo) {
            CHECKF(count("Bandit_Arsonist_FireboltCombo") >= 2, "it casts its combo now and then (%zu)", count("Bandit_Arsonist_FireboltCombo"));
        } else {
            CHECKF(count("Bandit_Arsonist_FireboltCombo") == 0, "its tree switches the combo off (%zu)", count("Bandit_Arsonist_FireboltCombo"));
        }
        CHECKF(count("Bandit_Arsonist_FlameBurst") + count("Bandit_Arsonist_FlameThrow") + count("Bandit_Arsonist_FlameRing") == 0, "it never casts its spray, stream or ring of fire (%zu, %zu, %zu)",
               count("Bandit_Arsonist_FlameBurst"), count("Bandit_Arsonist_FlameThrow"), count("Bandit_Arsonist_FlameRing"));
        // a cast is done at its montage's animation end (1.0 s) and the tree's global timer of 1 s runs from there: casts come 2.0 s apart, plus the 0.2 s the creature looks at
        std::vector<double> gaps;
        for (size_t i = 1; i < casts.size(); ++i) gaps.push_back(casts[i].t - casts[i - 1].t);
        std::sort(gaps.begin(), gaps.end());
        if (!gaps.empty()) std::printf("  cast to cast: %zu gaps, %.2f s at least, %.2f s in the middle, %.2f s at most\n", gaps.size(), gaps.front(), gaps[gaps.size() / 2], gaps.back());
        CHECKF(gaps.size() >= 12 && gaps.front() >= 2.0 - 1e-6 && gaps.back() <= 2.4, "casts come 2.0 to 2.4 s apart (%.2f to %.2f s)", gaps.empty() ? 0.0 : gaps.front(), gaps.empty() ? 0.0 : gaps.back());
        // the combo waits for the tree's timers: its first after 2 to 6 s of the fight, then at least its own 4.5 s and the 5 to 7 s its timer takes to charge again after the cast is done
        const std::vector<double>& combos = by_name["Bandit_Arsonist_FireboltCombo"];
        if (!casts.empty() && !combos.empty()) CHECKF(combos.front() - casts.front().t >= 1.7, "the first combo comes after the 2nd second of the fight (%.2f s after the first cast)", combos.front() - casts.front().t);
        for (size_t i = 1; i < combos.size(); ++i) CHECKF(combos[i] - combos[i - 1] >= 5.99, "two combos are at least 6 s apart (the cast's 1 s and the timer's 5 s: %.2f s)", combos[i] - combos[i - 1]);
        // every bolt leaves at its notify and lands when it arrives; nothing lands that was not launched
        const ShotReport report = shot_report(f, tables, casts, hits, kSeconds, 1.5);
        double single_min = 1e9, single_max = 0;
        for (const ShotCast& c : casts) {
            if (c.name != "Bandit_Arsonist_Firebolt") continue;
            const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
            if (!md || md->fires.size() != 1) continue;
            const double leaves = c.t + md->fires.front().time / md->rate_scale;
            for (const combat::IncomingHit& h : hits) {
                if (h.source_type == content::kSourceTypeProjectile && h.at >= leaves && h.at <= leaves + 1.5) {
                    single_min = std::min(single_min, h.at - c.t);
                    single_max = std::max(single_max, h.at - c.t);
                    break;
                }
            }
        }
        std::printf("  %zu bolts due, %zu landed; a single bolt landed %.2f to %.2f s after its cast; %zu hits without a bolt, %zu hits not made by a projectile\n", report.due, report.landed, single_min, single_max, report.stray,
                    report.not_projectile);
        CHECKF(report.due >= 20 && report.landed == report.due, "every bolt lands (%zu of %zu)", report.landed, report.due);
        CHECKF(report.stray == 0 && report.not_projectile == 0, "no hit comes without a bolt, and every hit names its projectile as its source (%zu, %zu)", report.stray, report.not_projectile);
        CHECKF(single_min >= 1.15 && single_max <= 1.45, "a bolt lands 1.15 to 1.45 s after its cast, with a cast 0.57 s long before it leaves and 20 m to fly (%.2f to %.2f s)", single_min, single_max);
        CHECKF(report.earliest_after_launch >= 0.4, "a bolt takes time to arrive (the quickest %.2f s after it left)", report.earliest_after_launch);
        // it stands where it is (the player is inside its 25 m) and faces him
        const LungeSample& last = samples.back();
        CHECKF(std::hypot(last.wx, last.wy) < 60.0, "it stays where it stood (%.0f cm from its place)", std::hypot(last.wx, last.wy));
    }
    // a player 33 m away: it casts as soon as he is within 30 m, the reach its tree lets the bolt go from, and closes in to its tree's arrival distance of 25 m between two casts
    {
        const std::string path = creature_path(f, "/Bandit_Arsonist/Bandit_Arsonist.Default__Bandit_Arsonist_C");
        const auto bp = f.motion.find(path);
        BanditOpen made;
        if (path.empty() || !bp || !make_bandit_open(f, tables, path, made)) return;
        std::vector<LungeSample> samples;
        CHECKF(run_lunge_fight(f, tables, path, 25.0, 0, 3300, nullptr, samples, nullptr, false, 0.5, nullptr, 40000.0, made.open), "a fight against a player 33 m away runs");
        double first_gap = -1, second_gap = -1, nearest = 1e9;
        for (const LungeSample& s : samples) {
            const double gap = std::hypot(s.wx - s.px, s.wy - s.py) - bp->radius - 34.0;
            nearest = std::min(nearest, gap);
            if (!s.cast_began) continue;
            if (first_gap < 0) first_gap = gap;
            else if (second_gap < 0) second_gap = gap;
        }
        std::printf("  from 33 m: its first cast began %.0f cm from the player's surface, its second %.0f cm, it came as near as %.0f cm\n", first_gap, second_gap, nearest);
        CHECKF(first_gap >= 2800.0 && first_gap <= 3005.0, "it begins casting once the player is within 30 m, the reach of its bolt (%.0f cm)", first_gap);
        CHECKF(second_gap >= 2400.0 && second_gap <= 2560.0, "between its first two casts it closes in to 25 m, its tree's arrival distance (%.0f cm)", second_gap);
        CHECKF(nearest >= 2400.0, "it does not come nearer than that (%.0f cm)", nearest);
    }
}

AOC_REGISTER_TEST(test_arsonist_fires_at_a_standing_player);

void test_plain_goblin_shaman_throws_its_bomb(Fixture& f) {
    std::printf("a plain goblin shaman against a player 2.5 m away: its oozing bomb leaves at its montage's notify, flies, and lands its damage when it arrives (it was an instant hit)\n");
    constexpr double kSeconds = 30.0;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/GoblinShaman/Goblin_Shaman.Default__Goblin_Shaman_C");
    const auto bp = f.motion.find(path);
    CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the plain goblin shaman and the creature tables list its abilities");
    if (path.empty() || !bp || !tables.abilities_of(path)) return;
    BanditOpen made;
    CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the shaman (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    CHECKF(run_lunge_fight(f, tables, path, kSeconds, 0, 250, nullptr, samples, &hits, false, 0.5, nullptr, 40000.0, made.open), "a fight of %.0f s against a player 2.5 m away runs", kSeconds);
    if (samples.empty()) return;
    const std::vector<ShotCast> casts = shot_casts(f, samples);
    size_t bombs = 0;
    for (const ShotCast& c : casts) bombs += c.name == "GoblinShaman_OozingBomb" ? 1 : 0;
    const ShotReport report = shot_report(f, tables, casts, hits, kSeconds, 1.0);
    std::printf("  %zu casts, %zu of them the bomb; %zu bombs due, %zu landed, the quickest %.2f s after it left; %zu hits without a bomb\n", casts.size(), bombs, report.due, report.landed, report.earliest_after_launch, report.stray);
    CHECKF(bombs >= 3, "it throws its bomb over and over (%zu)", bombs);
    CHECKF(report.due >= 3 && report.landed == report.due, "every bomb lands (%zu of %zu)", report.landed, report.due);
    size_t other_hits = 0;   // the hits that are not the bomb's must be its Bludgeon's (a melee swing at the player beside it)
    for (const combat::IncomingHit& h : hits) {
        if (h.source_type == content::kSourceTypeProjectile) continue;
        const abilities::AbilityDef* def = f.catalog->ability(h.source_guid);
        if (!def || def->row.name.find("Bludgeon") == std::string::npos) ++other_hits;
    }
    CHECKF(report.stray == 0 && other_hits == 0, "no hit comes without a bomb but the Bludgeon's swings (%zu hits of a projectile that never left, %zu of another ability)", report.stray, other_hits);
    // the bomb leaves at 1.8 s of its montage and lands at once at 2.5 m: no hit before the montage has played that far
    double earliest_after_cast = 1e9;
    for (double t : report.after_cast) earliest_after_cast = std::min(earliest_after_cast, t);
    CHECKF(earliest_after_cast >= 1.8, "no damage comes before the bomb has left the shaman's hand (%.2f s after the cast)", earliest_after_cast);
}

AOC_REGISTER_TEST(test_plain_goblin_shaman_throws_its_bomb);

void test_goblin_flayer_animates_its_attacks(Fixture& f) {
    std::printf("a Goblin Flayer (made from a generic Blueprint) against a player who stands still: with its tree's abilities in its open it plays its overhead slashes and its cleave, lunges at him along "
                "their root motion (unreadable before), lands each hit at its notify and stays about him; with the donor's own list of records it keeps the single swing\n");
    check_generic_creature_animates(f, {"/GoblinFlayer/Goblin_Flayer_Gravehide.Default__Goblin_Flayer_Gravehide_C", "Gravehide Flayer", 60.0, false, 3, 100.0, 8, 6}, [](const std::map<std::string, size_t>& by_name) {
        const auto count = [&](const char* name) { const auto it = by_name.find(name); return it == by_name.end() ? size_t{0} : it->second; };
        CHECKF(count("GoblinFlayer_OverheadSlash") + count("GoblinFlayer_OverheadSlash2") >= 4, "it plays the overhead slashes, whose root motion could not be read before (%zu + %zu)", count("GoblinFlayer_OverheadSlash"),
               count("GoblinFlayer_OverheadSlash2"));
    });
}

AOC_REGISTER_TEST(test_goblin_flayer_animates_its_attacks);

void test_languid_goblin_animates_its_swing(Fixture& f) {
    std::printf("a Languid Goblin (made from a generic Blueprint, whose only ability was missing from the ability data) that a player hits: it plays its melee swing, lunges at him along the montage's root motion, lands each "
                "hit at its notify and stays about him; with the donor's own list of records it keeps the single swing\n");
    check_generic_creature_animates(f, {"/Goblin/Goblin_LanguidGoblin.Default__Goblin_LanguidGoblin_C", "Languid Goblin", 40.0, true, 2, 150.0, 6, 6}, [](const std::map<std::string, size_t>& by_name) {
        size_t all = 0, swings = 0;
        for (const auto& [label, n] : by_name) {
            all += n;
            if (label == "Goblin_Languid_Swing") swings = n;
        }
        CHECKF(swings >= 6 && swings == all, "every cast is its swing (%zu of %zu casts)", swings, all);
    });
}

AOC_REGISTER_TEST(test_languid_goblin_animates_its_swing);

void test_humanoids_walk_up_to_a_far_player(Fixture& f) {
    std::printf("a goblin, a bandit raider and a bandit captain against a player 25 m away: they run at their Blueprint's run speed times their MoveSpeedMult and walk up to the nearest reach of the attacks they can use "
                "(a special that waits for a timer does not park them), a wolf and a bear keep their Blueprint's speed\n");
    constexpr double kDt = 1.0 / 15.0, kStartGap = 2500.0;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    struct Kind {
        const char* leaf;
        bool        humanoid;
    };
    for (const Kind kind : {Kind{"/Bandit_Raider/CSB_Bandit_Raider_StateTree.Default__CSB_Bandit_Raider_StateTree_C", true}, Kind{"/Bandit_Raider/Bandit_Raider.Default__Bandit_Raider_C", true},
                            Kind{"/BanditCaptain/Bandit_Captain.Default__Bandit_Captain_C", true}, Kind{"/Goblin/Goblin_Gravehide.Default__Goblin_Gravehide_C", true},
                            Kind{"/Canine/Wolf.Default__Wolf_C", false}, Kind{"/Bear/Bear.Default__Bear_C", false}}) {
        const std::string path = creature_path(f, kind.leaf);
        const auto bp = f.motion.find(path);
        const std::string name = spawns::blueprint_leaf(path);
        CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the %s and the creature tables list its abilities", name.c_str());
        if (path.empty() || !bp || !tables.abilities_of(path)) return;
        BanditOpen made;
        if (kind.humanoid) {
            CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the %s", name.c_str());
            if (!made.open) return;
        }
        std::string err;
        auto cs = book.make(*bp, 7, err);
        const stats::StatValue* mult = cs ? cs->find(f.data->ids().move_speed_mult) : nullptr;
        CHECKF(mult && mult->value >= 1.0, "the %s has a MoveSpeedMult of its own at level 7 (%.2f)", name.c_str(), mult ? mult->value : 0.0);
        if (!mult) return;
        const double speed = kind.humanoid ? bp->max_run_speed * mult->value : bp->max_run_speed;
        std::vector<LungeSample> samples;
        CHECKF(run_lunge_fight(f, tables, path, 25.0, 0, kStartGap, nullptr, samples, nullptr, false, 0.5, nullptr, 60000.0, made.open), "a fight of 25 s against a player 25 m away runs for the %s", name.c_str());
        if (samples.empty()) return;
        const ChaseNumbers n = chase_numbers(samples, kDt);
        std::printf("  %-34s set off %.2f s, speed %.0f cm/s (Blueprint run %.0f x MoveSpeedMult %.2f), first cast %.2f s from %.0f cm\n", name.c_str(), n.first_move, n.speed, bp->max_run_speed, mult->value, n.first_cast,
                    n.gap_at_cast);
        check_chase(name.c_str(), n, kStartGap, speed, kind.humanoid);
    }
}

AOC_REGISTER_TEST(test_humanoids_walk_up_to_a_far_player);

/// The same on the real ground: opt-in (AOC_REAL_GROUND=1 reads dist/content/area_nav.bin and loads the chunks round the camps).
void test_humanoids_walk_up_on_the_real_ground(Fixture& f) {
    const char* env = std::getenv("AOC_REAL_GROUND");
    if (!env || std::string(env) != "1") {
        std::printf("humanoid creatures walking up to a player on the real ground: opt-in (AOC_REAL_GROUND=1 reads dist/content/area_nav.bin)\n");
        return;
    }
    std::printf("goblins, a bandit raider and a bandit captain on the real ground: each reaches a player standing 20 m away along a real path, at its speed, without standing and staring\n");
    constexpr double kDt = 1.0 / 15.0, kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string err;
    auto nav = nav::AreaNav::load(dir + "area_nav.bin", &err);
    if (!nav) {
        std::printf("  [skip] the real ground: %s\n", err.c_str());
        return;
    }
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    struct Camp {
        const char* leaf;
        double      x, y;
    };
    for (const Camp camp : {Camp{"/Goblin/Goblin_Gravehide.Default__Goblin_Gravehide_C", -796495, 569503}, Camp{"/Bandit_Raider/Bandit_Raider.Default__Bandit_Raider_C", -837337, 640605},
                            Camp{"/BanditCaptain/Bandit_Captain.Default__Bandit_Captain_C", -837337, 640605}}) {
        const std::string path = creature_path(f, camp.leaf);
        const auto bp = f.motion.find(path);
        const std::string name = spawns::blueprint_leaf(path);
        if (path.empty() || !bp || !tables.abilities_of(path)) return;
        if (!nav->load_around(camp.x, camp.y, 2, 180)) {
            std::printf("  [skip] the ground round the %s's camp did not load\n", name.c_str());
            continue;
        }
        nav::Vec3 home;
        if (!nav->nearest_point({camp.x, camp.y, 12000.0}, 3000, 100000, &home)) continue;
        BanditOpen made;
        if (!make_bandit_open(f, tables, path, made)) return;
        std::string e2;
        auto cs = book.make(*bp, 7, e2);
        const stats::StatValue* mult = cs ? cs->find(f.data->ids().move_speed_mult) : nullptr;
        if (!mult) return;
        const double speed = bp->max_run_speed * mult->value;
        size_t runs = 0;
        for (int d = 0; d < 8 && runs < 3; ++d) {
            const double a = d * kPi / 4.0;
            const nav::Vec3 want{home.x + 2000.0 * std::cos(a), home.y + 2000.0 * std::sin(a), home.z};
            nav::Vec3 target;
            if (!nav->nearest_point(want, 400, 600, &target) || std::hypot(target.x - want.x, target.y - want.y) > 250.0) continue;
            const nav::PathResult route = nav->find_path(home, target, 200, 400, 20000);
            if (!route.found || !route.complete || route.length > 2300.0) continue;   // (a spot the creature can reach by a path about as long as the straight line)
            Rig rig(4, 20, {});
            if (!rig.ok()) return;
            combat_rig::World w;
            if (!w.problem.empty()) return;
            w.npcs = std::make_unique<npc::NpcWorld>(w.registry, nav.get());
            combat::CombatWorldConfig cfg;
            cfg.seed = 5;
            combat::CombatWorld world(w.registry, w.npcs.get(), nav.get(), *f.data, *f.catalog, book, f.motion, cfg);
            world.set_ability_data(&tables);
            const uint64_t creature = combat_rig::spawn_npc(w.registry, w.npcs.get(), *w.captured, w.next_id, path, home.x, home.y, home.z + bp->half_height * bp->default_scale, 7, 900, bp->half_height, made.open);
            if (creature == 0) return;
            world::WorldActor* actor = w.registry.find(creature);
            actor->npc.leash_radius_cm = 60000.0;
            combat_rig::FakePlayer player(rig, 77, {target.x, target.y, target.z + 98.0});
            world.add_player(&player);
            std::vector<LungeSample> samples;
            GremCastView last;
            double now = 0, next_hit = 0.5;
            while (now < 25.0) {
                now += kDt;
                if (now >= next_hit) {
                    next_hit = now + 1.0;
                    world.add_threat(creature, 77, 100.0, now);
                }
                if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
                nav->pump(50.0);
                w.npcs->note_player(77, target.x, target.y, now);
                w.npcs->tick(now);
                world.tick(now);
                const world::WorldActor* g = w.registry.find(creature);
                const GremCastView view = grem_cast_view(w.registry, creature);
                LungeSample s;
                s.t = now, s.wx = g->x, s.wy = g->y, s.px = target.x, s.py = target.y, s.montage = view.montage;
                s.cast_began = view.montage != 0 && (view.element != last.element || last.montage == 0);
                last = view;
                samples.push_back(s);
            }
            ++runs;
            const ChaseNumbers n = chase_numbers(samples, kDt);
            const double start_gap = std::hypot(samples.front().wx - target.x, samples.front().wy - target.y);
            std::printf("  %-26s direction %d: path %.0f cm, set off %.2f s, speed %.0f cm/s, first cast %.2f s from %.0f cm\n", name.c_str(), d, route.length, n.first_move, n.speed, n.first_cast, n.gap_at_cast);
            check_chase((name + " on the real ground").c_str(), n, start_gap, speed, true);
        }
        CHECKF(runs >= 1, "the %s has a real spot 20 m from its camp that it can walk to (%zu found)", name.c_str(), runs);
    }
}

AOC_REGISTER_TEST(test_humanoids_walk_up_on_the_real_ground);

// The tree's own walk speed during an attack that does not stop the creature: the raiders' Whirlwind walks at 0.45 of the creature's run speed (a rank 3 raider runs 810 cm/s and its
// whirlwind walk was recorded at 364-365), and the Goblin Flayer's SeveringSpin at 1.6.  The player is put 15 m off the tick the attack begins and runs on at 150 cm/s, so the creature's AI
// chases him for the whole montage, and what it covers in a tick is the speed the tree's move-to-target task walks it at.
struct AttackWalk {
    bool   ran = false, cast = false;
    size_t ticks = 0;
    double speed = 0, chase = 0;
    std::map<std::string, size_t> casts;
};

static AttackWalk attack_walk(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, const spawns::OpenTemplate* open, const abilities::AbilityDef& ability,
                              double seconds, double chase_cm_s) {
    constexpr double kDt = 1.0 / 15.0;
    AttackWalk out;
    out.chase = chase_cm_s;
    bool away = false;
    double dirx = 0, diry = 0;
    const LungeSteer steer = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& c) {
        if (!away) {
            if (!prev || !prev->cast_began || prev->ability != ability.row.guid) return false;
            // (the tick its attack begins, the player is put 15 m off along the way it faces and runs on: its AI has to walk after him for the whole montage)
            const double bearing = std::atan2(py - c.y, px - c.x);
            dirx = std::cos(bearing);
            diry = std::sin(bearing);
            px = c.x + 1500.0 * dirx;
            py = c.y + 1500.0 * diry;
            away = true;
            return true;
        }
        px += 150.0 * kDt * dirx;
        py += 150.0 * kDt * diry;
        return true;
    };
    std::vector<LungeSample> samples;
    if (!run_lunge_fight(f, tables, path, seconds, 0, 800, steer, samples, nullptr, false, -1, nullptr, 40000.0, open)) return out;
    out.ran = true;
    for (const LungeSample& s : samples) {
        if (!s.cast_began) continue;
        const abilities::AbilityDef* def = f.catalog->ability(s.ability);
        if (def) ++out.casts[def->row.name];
    }
    std::vector<double> speeds;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (!samples[i].cast_began || samples[i].ability != ability.row.guid) continue;
        out.cast = true;
        size_t last = i;   // the end of that cast
        while (last + 1 < samples.size() && samples[last + 1].montage != 0 && !samples[last + 1].cast_began) ++last;
        for (size_t k = i + 1; k <= last; ++k) {
            const double step = std::hypot(samples[k].wx - samples[k - 1].wx, samples[k].wy - samples[k - 1].wy) / kDt;
            if (step > 20.0) speeds.push_back(step);   // (a tick it did not walk: the player was still inside the reach of the attack it is ready with)
        }
        break;   // (the first one is enough)
    }
    std::sort(speeds.begin(), speeds.end());
    out.ticks = speeds.size();
    out.speed = speeds.empty() ? 0.0 : speeds[speeds.size() / 2];
    return out;
}

void test_attack_walk_speeds(Fixture& f) {
    std::printf("the tree's walk during an attack that does not stop the creature: the bandit raiders' Whirlwind walks at 0.45 of its chase speed and the Goblin Flayer's SeveringSpin at 1.6\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    struct Kind {
        const char* leaf;
        const char* ability;
        double      share;
        double      seconds;
    };
    for (const Kind kind : {Kind{"/Bandit_Raider/CSB_Bandit_Raider_StateTree.Default__CSB_Bandit_Raider_StateTree_C", "Bandit_Raider_Whirlwind", 0.45, 90.0},
                            Kind{"/GoblinFlayer/Goblin_Flayer_Gravehide.Default__Goblin_Flayer_Gravehide_C", "GoblinFlayer_SeveringSpin", 1.6, 90.0}}) {
        const std::string path = creature_path(f, kind.leaf);
        const auto bp = f.motion.find(path);
        const abilities::AbilityDef* ability = f.catalog->ability_named(kind.ability);
        const std::string name = spawns::blueprint_leaf(path);
        CHECKF(!path.empty() && bp && tables.abilities_of(path) && ability, "the Blueprint table has the %s, the creature tables list its abilities and the ability data has the %s", name.c_str(),
               kind.ability);
        if (path.empty() || !bp || !tables.abilities_of(path) || !ability) continue;
        const combat::CreatureAbility* task = nullptr;   // the task that plays the attack
        for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
            if (t.ability == ability->row.guid) task = &t;
        }
        CHECKF(task && !task->stop_movement, "the %s's tree plays the %s without stopping it (stop_movement %d)", name.c_str(), kind.ability, task ? (task->stop_movement ? 1 : 0) : -1);
        if (!task || task->stop_movement) continue;
        BanditOpen made;
        CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the %s (verdict: %s)", name.c_str(), spawns::verdict_name(made.why));
        if (!made.open) continue;
        std::string err;
        auto cs = book.make(*bp, 7, err);
        const stats::StatValue* mult = cs ? cs->find(f.data->ids().move_speed_mult) : nullptr;
        CHECKF(mult && mult->value >= 1.0, "the %s has a MoveSpeedMult of its own at level 7 (%.2f)", name.c_str(), mult ? mult->value : 0.0);
        if (!mult) continue;
        const double chase = std::max({bp->max_run_speed, bp->max_walk_speed, 300.0}) * mult->value;
        const AttackWalk walk = attack_walk(f, tables, path, made.open, *ability, kind.seconds, chase);
        CHECKF(walk.ran && walk.cast, "a fight of %.0f s in which the %s plays its %s runs", kind.seconds, name.c_str(), kind.ability);
        if (!walk.ran || !walk.cast) continue;
        std::string seen;
        for (const auto& [label, n] : walk.casts) seen += (seen.empty() ? "" : ", ") + label + " " + std::to_string(n);
        const double ratio = walk.speed / chase;
        std::printf("  %s: %s; while its montage plays it walks %.0f cm/s (%.2f x its chase speed of %.0f cm/s) over %zu ticks\n", name.c_str(), seen.c_str(), walk.speed, ratio, chase, walk.ticks);
        CHECKF(walk.ticks >= 8 && std::fabs(ratio - kind.share) <= 0.05 * kind.share + 0.02, "the %s walks at %.2f x its chase speed while the %s plays (%.2f x measured over %zu ticks)", name.c_str(),
               kind.share, kind.ability, ratio, walk.ticks);
    }
}

AOC_REGISTER_TEST(test_attack_walk_speeds);

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_sequence_tests.inc
//
//  Registered tests of test_worldd_combat: the moves a creature's state tree plays one after the other.  The reader writes, for a task that starts a cooldown another later task of
//  the same tree waits on, the later task's ability in the table's `next` column; the world plays the first of those tasks whose own conditions hold when the cast that readied it is
//  over, and the cooldown that links the pair (the finished move's own timer or the next move's) is open for it.  A cooldown both moves share stays shut.  A chain partner the tables
//  hold back is not played and the first move is played alone: nothing waits on it.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

// These checks print their own passing line too, so the proof of the eight angles can quote the [ok ] line of a check as well as its [FAIL] line.
// (One printf for the whole line: the world's log writes to the same console between two calls.)
#define SEQ_CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define SEQ_CHECKF(cond, ...) do { \
    char seq_line[1024]; \
    std::snprintf(seq_line, sizeof seq_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", seq_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", seq_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace sequences {

/// One row of the real table as a test changes it: the `next` links it gets, and the columns whose text it replaces.
struct RowEdit {
    int64_t                                                ability = 0;
    std::string                                            next;
    std::vector<std::pair<size_t, std::string>>            fields;
};

/// How the table a test writes carries the sequence: every row's `next` empty (the controlled table a chain test needs), the column the build's table has as it is, or no column at all
/// (the header every table had before the sequences).
enum class NextColumn { Empty, Keep, None };

/// The real creature table written to a temp file with the sequence column the test wants: `next` empty unless an edit names its ability, the build's own column, or left out.  An edit may
/// replace cells (the Bear's Pound, which the reader cannot say which target its distance check reads until the target a Rotate To stored is read).
static std::string table_with_next(const std::string& dir, const std::string& archetype, const std::vector<RowEdit>& edits, std::string& error,
                                   NextColumn mode = NextColumn::Empty) {
    std::ifstream in(dir + "npc_abilities.tsv", std::ios::binary);
    if (!in) {
        error = "npc_abilities.tsv cannot be opened";
        return "";
    }
    const std::filesystem::path out = std::filesystem::temp_directory_path() / ("aoc_sequences_" + unique_temp_suffix() + ".tsv");
    std::ofstream o(out, std::ios::binary);
    std::string line;
    bool first = true;
    size_t next_at = 0;
    const auto cells_of = [](const std::string& text) {
        std::vector<std::string> c;
        size_t from = 0;
        while (true) {
            const size_t tab = text.find('\t', from);
            c.push_back(text.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
            if (tab == std::string::npos) return c;
            from = tab + 1;
        }
    };
    const auto write_cells = [&o](const std::vector<std::string>& c, size_t skip) {
        bool first_cell = true;
        for (size_t i = 0; i < c.size(); ++i) {
            if (i == skip) continue;
            if (!first_cell) o << '\t';
            o << c[i];
            first_cell = false;
        }
        o << '\n';
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> c = cells_of(line);
        if (first) {
            first = false;
            next_at = c.size();
            for (size_t i = 0; i < c.size(); ++i) {
                if (c[i] == "next") next_at = i;
            }
            if (mode == NextColumn::None) {
                write_cells(c, next_at);
            } else if (next_at == c.size()) {
                o << line << "\tnext\n";
            } else {
                o << line << '\n';
            }
            continue;
        }
        std::string next = mode != NextColumn::Empty && next_at < c.size() ? c[next_at] : std::string();
        if (c.size() > 1 && c[0] == archetype) {
            for (const RowEdit& e : edits) {
                if (std::strtoll(c[1].c_str(), nullptr, 10) != e.ability) continue;
                next = e.next;
                for (const auto& [column, text] : e.fields) {
                    if (column < c.size()) c[column] = text;
                }
            }
        }
        if (mode == NextColumn::None) {
            write_cells(c, next_at);
        } else {
            for (size_t i = 0; i < next_at && i < c.size(); ++i) o << c[i] << '\t';
            o << next << '\n';
        }
    }
    if (first) {
        error = "npc_abilities.tsv is empty";
        return "";
    }
    return out.string();
}

/// A fight of one creature whose tree is the real one with a sequence column, against a player who stands where the test puts him.
struct SequenceFight {
    bool                             ran = false;
    std::vector<LungeSample>         samples;
    std::vector<combat::IncomingHit> hits;
};

static bool run_sequence_fight(Fixture& f, const std::string& table, const std::string& path, double seconds, double player_x, double player_y, SequenceFight& out,
                               const LungeStep& on_step = nullptr) {
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    combat::NpcAbilityData tables;
    std::string err;
    const bool loaded = tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
    if (loaded) {
        std::printf("  [ok ] the tree with its sequence column loads\n");
        std::fflush(stdout);
        ++g_pass;
    } else {
        std::printf("  [FAIL] the tree with its sequence column loads (%s)\n", err.c_str());
        std::fflush(stdout);
        ++g_fail;
        return false;
    }
    SEQ_CHECKF(f.motion.find(path) && tables.abilities_of(path), "the Blueprint table has the creature and the tree with its sequence column lists its abilities");
    if (!f.motion.find(path) || !tables.abilities_of(path)) return false;
    BanditOpen made;
    SEQ_CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the creature (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return false;
    out.ran = run_lunge_fight(f, tables, path, seconds, player_x, player_y, nullptr, out.samples, &out.hits, false, -1, nullptr, 40000.0, made.open, 0, 0, on_step);
    SEQ_CHECK(out.ran, "the fight runs");
    return out.ran;
}

/// A cast of the fight as the client saw it begin: when, its ability, and how long its montage played (summed from the ticks it was open).
struct Cast {
    double  t = 0, seconds = 0;
    int64_t ability = 0;
};

static std::vector<Cast> casts_of(const std::vector<LungeSample>& samples, double dt) {
    std::vector<Cast> out;
    for (const LungeSample& s : samples) {
        if (s.cast_began) out.push_back({s.t, 0.0, s.ability});
        if (!out.empty() && s.montage != 0 && s.ability == out.back().ability) out.back().seconds += dt;
    }
    return out;
}

}  // namespace sequences

// R3: the Goblin Flayer's Manic Sneer starts the cooldown its Severing Spin waits on.  With the chain the tree plays the pair, not the spin alone: in a 90 s fight the sneer comes
// twice and the spin follows each at once, 45 s apart (the cooldown the pair starts), and the spin is never starved by the cooldown the sneer started.
void test_flayer_sneer_then_spin(Fixture& f) {
    std::printf("the Goblin Flayer's sneer plays, its spinning attack follows at once as the tree's chain says, the pair comes again after the 45 s it starts, and the spin is never starved\n");
    const std::string path = creature_path(f, "/Humanoid/Goblin/GoblinSlaver/GoblinFlayer/Goblin_Flayer.Default__Goblin_Flayer_C");
    const abilities::AbilityDef* sneer = f.catalog->ability_named("GoblinFlayer_ManicSneer");
    const abilities::AbilityDef* spin = f.catalog->ability_named("GoblinFlayer_SeveringSpin");
    SEQ_CHECK(!path.empty() && f.motion.find(path) && sneer && spin, "the Blueprint table has the Flayer and the ability data has its Manic Sneer and Severing Spin");
    if (path.empty() || !f.motion.find(path) || !sneer || !spin) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string error;
    const std::string table = sequences::table_with_next(dir, path, {{sneer->row.guid, std::to_string(spin->row.guid), {}}}, error);
    SEQ_CHECKF(!table.empty(), "the Flayer's tree with its sequence column is written (%s)", error.c_str());
    if (table.empty()) return;
    sequences::SequenceFight fight;
    if (!sequences::run_sequence_fight(f, table, path, 92.0, 0, 800, fight)) return;
    const std::vector<sequences::Cast> casts = sequences::casts_of(fight.samples, 1.0 / 15.0);
    std::string line;
    for (const sequences::Cast& c : casts) {
        const abilities::AbilityDef* d = f.catalog->ability(c.ability);
        line += (line.empty() ? "" : ", ") + std::string(d ? d->row.name : "?") + " " + std::to_string(c.t).substr(0, 5);
    }
    std::printf("  casts: %s\n", line.c_str());
    std::vector<double> sneers, spins;
    for (const sequences::Cast& c : casts) {
        if (c.ability == sneer->row.guid) sneers.push_back(c.t);
        if (c.ability == spin->row.guid) spins.push_back(c.t);
    }
    std::printf("  sneers at:");
    for (const double t : sneers) std::printf(" %.2f", t);
    std::printf(" s; spins at:");
    for (const double t : spins) std::printf(" %.2f", t);
    std::printf(" s\n");
    SEQ_CHECKF(sneers.size() >= 2, "the sneer plays, and again after its cooldown, in a 92 s fight (%zu)", sneers.size());
    SEQ_CHECKF(spins.size() >= 2, "the spin plays with it, and again, in a 92 s fight (%zu)", spins.size());
    if (sneers.size() < 2 || spins.size() < 2) return;
    // the spin follows its sneer at once (the sneer's 3.17 s montage, then the chain), and never the other way round
    SEQ_CHECKF(spins.front() >= sneers.front(), "the spin does not come before the sneer it is chained to (sneer %.2f s, spin %.2f s)", sneers.front(), spins.front());
    SEQ_CHECKF(spins.front() - sneers.front() <= 3.7, "the spin follows the sneer as its chain says: it begins %.2f s after the sneer (the sneer's montage is 3.17 s)", spins.front() - sneers.front());
    SEQ_CHECKF(sneers.size() < 2 || sneers[1] - sneers[0] >= 40.0, "the pair comes again only after the cooldown the sneer starts (45 s): the second sneer is %.2f s after the first", sneers[1] - sneers[0]);
    SEQ_CHECKF(sneers.size() < 2 || spins.size() < 2 || spins[1] >= sneers[1], "the second spin is not starved by the cooldown the first pair started (second spin at %.2f s, second sneer at %.2f s)", spins[1], sneers[1]);
}

AOC_REGISTER_TEST(test_flayer_sneer_then_spin);

// R4: the Bear's Rear Up is followed by its Pound, and the Pound's hit lands on the player.  The table gives the Rear Up the Pound as its next move; the Pound's distance check
// reads the target the Rear Up's Rotate To stored (the player), which the reader now says as the primary target.
void test_bear_rear_up_then_pound(Fixture& f) {
    std::printf("a bear rears up and its Pound follows at once as the tree's chain says, and the Pound's hit lands on the player who stands 2.5 m away\n");
    const std::string path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    const abilities::AbilityDef* rear = f.catalog->ability_named("Bear_RearUp");
    const abilities::AbilityDef* pound = f.catalog->ability_named("Bear_Pound");
    SEQ_CHECK(!path.empty() && f.motion.find(path) && rear && pound, "the Blueprint table has the Bear and the ability data has its Rear Up and Pound");
    if (path.empty() || !f.motion.find(path) || !rear || !pound) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string error;
    sequences::RowEdit pound_row;
    pound_row.ability = pound->row.guid;
    // the real Pound row's condition: its distance check reads the target the Rear Up's Rotate To stored (the reader writes it as the primary target once it follows the
    // stored target; the build's table still holds it as `other target`)
    pound_row.fields = {{10, "0"}, {12, "c0:fc(ge,10)|c0:in(500) o0:T"}, {13, ""}};
    const std::string table = sequences::table_with_next(dir, path, {{rear->row.guid, std::to_string(pound->row.guid), {}}, pound_row}, error);
    SEQ_CHECKF(!table.empty(), "the Bear's tree with its sequence column is written (%s)", error.c_str());
    if (table.empty()) return;
    sequences::SequenceFight fight;
    if (!sequences::run_sequence_fight(f, table, path, 45.0, 0, 250, fight)) return;
    const std::vector<sequences::Cast> casts = sequences::casts_of(fight.samples, 1.0 / 15.0);
    std::string line;
    for (const sequences::Cast& c : casts) {
        const abilities::AbilityDef* d = f.catalog->ability(c.ability);
        line += (line.empty() ? "" : ", ") + std::string(d ? d->row.name : "?") + " " + std::to_string(c.t).substr(0, 5);
    }
    std::printf("  casts: %s\n", line.c_str());
    size_t rear_at = casts.size(), pound_at = casts.size();
    for (size_t i = 0; i < casts.size(); ++i) {
        if (casts[i].ability == rear->row.guid && rear_at == casts.size()) rear_at = i;
        if (casts[i].ability == pound->row.guid && rear_at != casts.size() && pound_at == casts.size()) pound_at = i;
    }
    SEQ_CHECKF(rear_at != casts.size(), "the bear rears up in a 45 s fight");
    if (rear_at == casts.size()) return;
    SEQ_CHECKF(pound_at != casts.size(), "its Pound follows the Rear Up");
    if (pound_at == casts.size()) return;
    SEQ_CHECKF(pound_at == rear_at + 1, "the Pound is the very next move after the Rear Up (cast %zu after it)", pound_at - rear_at);
    SEQ_CHECKF(casts[pound_at].t - (casts[rear_at].t + casts[rear_at].seconds) <= 0.5, "the Pound begins at once when the Rear Up is over (%.2f s after its end)", casts[pound_at].t - (casts[rear_at].t + casts[rear_at].seconds));
    size_t pound_hits = 0;
    for (const combat::IncomingHit& h : fight.hits) {
        if (h.source_guid == pound->row.guid && h.at >= casts[pound_at].t && h.at <= casts[pound_at].t + casts[pound_at].seconds + 0.2) ++pound_hits;
    }
    SEQ_CHECKF(pound_hits >= 1, "the Pound's hit lands on the player during the Pound (%zu hits)", pound_hits);
}

AOC_REGISTER_TEST(test_bear_rear_up_then_pound);

// R3 (the shared gate): the bandit raider's tree links its Whirlwind to its Heavy Slash (the two specials share the "Special Attack" cooldown of 8 s).  The chain must not open a cooldown
// the two moves share: the Heavy Slash follows only once the shared cooldown is over, whatever the link says.  The link is in the build's table, so the fight is the real one.
void test_chain_step_keeps_a_shared_gate(Fixture& f) {
    std::printf("a bandit raider's Whirlwind is linked to its Heavy Slash through the 8 s cooldown the specials share, and the chain does not let the Heavy Slash through that gate\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Bandit_Raider/CSB_Bandit_Raider_StateTree.Default__CSB_Bandit_Raider_StateTree_C");
    const std::vector<combat::CreatureAbility>* tasks = path.empty() ? nullptr : tables.abilities_of(path);
    const abilities::AbilityDef* whirl = f.catalog->ability_named("Bandit_Raider_Whirlwind");
    const abilities::AbilityDef* heavy = f.catalog->ability_named("Bandit_Raider_HeavySlash");
    SEQ_CHECK(!path.empty() && tasks && whirl && heavy, "the Blueprint table has the bandit raider and the ability data has its Whirlwind and Heavy Slash");
    if (path.empty() || !tasks || !whirl || !heavy) return;
    bool linked = false;
    for (const combat::CreatureAbility& t : *tasks) {
        if (t.ability != whirl->row.guid) continue;
        linked = std::find(t.next.begin(), t.next.end(), heavy->row.guid) != t.next.end();
    }
    SEQ_CHECK(linked, "the table links the Whirlwind to the Heavy Slash as its next move");
    if (!linked) return;
    BanditOpen made;
    SEQ_CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the bandit raider (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    std::vector<LungeSample> samples;
    SEQ_CHECK(run_lunge_fight(f, tables, path, 60.0, 0, 800, nullptr, samples, nullptr, false, -1, nullptr, 40000.0, made.open), "a 60 s fight against a player who stands 8 m away runs");
    if (samples.empty()) return;
    const std::vector<sequences::Cast> casts = sequences::casts_of(samples, 1.0 / 15.0);
    std::vector<double> whirls, heavies;
    for (const sequences::Cast& c : casts) {
        if (c.ability == whirl->row.guid) whirls.push_back(c.t);
        if (c.ability == heavy->row.guid) heavies.push_back(c.t);
    }
    std::printf("  %zu whirls and %zu heavy slashes in 60 s\n", whirls.size(), heavies.size());
    SEQ_CHECKF(whirls.size() >= 1 && heavies.size() >= 2, "the raider whirls and heavy slashes in 60 s (%zu whirls, %zu heavy slashes)", whirls.size(), heavies.size());
    if (whirls.empty() || heavies.size() < 2) return;
    double closest = 1e9;
    for (const double heavy_at : heavies) {
        for (const double whirl_at : whirls) {
            if (heavy_at <= whirl_at) continue;
            closest = std::min(closest, heavy_at - whirl_at);
        }
    }
    SEQ_CHECKF(closest >= 8.0, "no Heavy Slash follows a Whirlwind before the shared 8 s cooldown is over (the closest after a Whirlwind is %.2f s)", closest >= 1e9 ? 0.0 : closest);
}

AOC_REGISTER_TEST(test_chain_step_keeps_a_shared_gate);

// R4 (the approach): the Bear's Pound is a state its tree enters after the Rear Up; its 24 m reach is not the distance the bear walks up to.  The bear walks to the reach of its own
// move and casts from there; it does not rear up 25 m from the player.
void test_bear_walks_to_its_own_reach(Fixture& f) {
    std::printf("a bear 25 m from a player walks up to the reach of its own attack and casts from there, not from the follow-up Pound's 24 m reach\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    SEQ_CHECK(!path.empty() && f.motion.find(path) && tables.abilities_of(path), "the Blueprint table has the Bear and the creature tables list its abilities");
    if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path)) return;
    std::vector<LungeSample> samples;
    SEQ_CHECK(run_lunge_fight(f, tables, path, 20.0, 0, 2500, nullptr, samples, nullptr, false, 0.5, nullptr, 60000.0), "a 20 s fight against a player 25 m away runs");
    if (samples.empty()) return;
    const ChaseNumbers n = chase_numbers(samples, 1.0 / 15.0);
    std::printf("  set off %.2f s, first cast %.2f s from %.0f cm\n", n.first_move, n.first_cast, n.gap_at_cast);
    SEQ_CHECKF(n.first_move >= 0 && n.first_move <= 1.0, "the bear sets off after the player at once (first move at %.2f s)", n.first_move);
    SEQ_CHECKF(n.first_cast >= 0 && n.first_cast <= 5.0 && n.gap_at_cast <= 700.0, "its first cast comes where it walked to, not across the 25 m (first cast %.2f s from %.0f cm)", n.first_cast,
               n.gap_at_cast);
}

AOC_REGISTER_TEST(test_bear_walks_to_its_own_reach);

// R5: the goblins' Rally is chained to its two Headbutts, and both are held back (their montages have no hit): the Rally plays alone exactly as before, and the fight goes on.
void test_goblin_rally_with_held_partners(Fixture& f) {
    std::printf("a goblin's Rally is chained to its two Headbutts, which the tables hold back: it rallies alone, never headbutts, and fights on\n");
    const std::string path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    const abilities::AbilityDef* rally = f.catalog->ability_named("Goblin_Rally");
    const abilities::AbilityDef* head = f.catalog->ability_named("Goblin_Headbutt");
    const abilities::AbilityDef* still = f.catalog->ability_named("Goblin_StationaryHeadbutt");
    SEQ_CHECK(!path.empty() && f.motion.find(path) && rally && head && still, "the Blueprint table has the Bonebinder goblin and the ability data has its Rally and Headbutts");
    if (path.empty() || !f.motion.find(path) || !rally || !head || !still) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string error;
    const std::string table = sequences::table_with_next(dir, path, {{rally->row.guid, std::to_string(head->row.guid) + ";" + std::to_string(still->row.guid), {}}}, error);
    SEQ_CHECKF(!table.empty(), "the goblin's tree with its sequence column is written (%s)", error.c_str());
    if (table.empty()) return;
    {
        // the two chain partners are held, as the real table says (nothing waits on them: the chain is dropped)
        combat::NpcAbilityData loaded;
        const bool goblin_loaded = loaded.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", error);
        if (goblin_loaded) {
            std::printf("  [ok ] the goblin's tree with its sequence column loads\n");
            std::fflush(stdout);
            ++g_pass;
        } else {
            std::printf("  [FAIL] the goblin's tree with its sequence column loads (%s)\n", error.c_str());
            std::fflush(stdout);
            ++g_fail;
            return;
        }
        if (!loaded.abilities_of(path)) return;
        for (const combat::CreatureAbility& t : *loaded.abilities_of(path)) {
            if (t.ability != head->row.guid && t.ability != still->row.guid) continue;
            SEQ_CHECKF(t.unknown && t.why.find("hit notify") != std::string::npos, "the chain partner %s is held back as before (%s)", f.catalog->ability(t.ability)->row.name.c_str(), t.why.c_str());
        }
    }
    sequences::SequenceFight fight;
    std::vector<size_t> chains;
    const LungeStep on_step = [&chains](LungeStepContext& c) {
        if (c.after) chains.push_back(c.world.npc_chain_size(c.creature));
    };
    if (!sequences::run_sequence_fight(f, table, path, 85.0, 0, 800, fight, on_step)) return;
    const std::vector<sequences::Cast> casts = sequences::casts_of(fight.samples, 1.0 / 15.0);
    size_t rallies = 0, headbutts = 0, before = 0, after = 0;
    double first_rally = -1;
    std::vector<double> rally_times;
    for (const sequences::Cast& c : casts) {
        if (c.ability == rally->row.guid) {
            ++rallies;
            rally_times.push_back(c.t);
            if (first_rally < 0) first_rally = c.t;
        } else if (c.ability == head->row.guid || c.ability == still->row.guid) {
            ++headbutts;
        } else if (first_rally < 0) {
            ++before;
        } else {
            ++after;
        }
    }
    std::printf("  %zu casts before the first Rally, the first Rally at %.2f s, %zu after it, %zu headbutts\n", before, first_rally, after, headbutts);
    SEQ_CHECKF(rallies >= 1, "the goblin rallies (%zu)", rallies);
    SEQ_CHECKF(headbutts == 0, "it never plays the held Headbutts its Rally is chained to (%zu)", headbutts);
    SEQ_CHECKF(before >= 3, "it fights from the start with the moves it had before (%zu casts before the Rally)", before);
    SEQ_CHECKF(after >= 3, "it fights on after the Rally: nothing waits on the held chain partner (%zu casts after it)", after);
    // the Rally's chain is set when its cast ends and the tree's next look consumes it: a partner the table holds drops it there (without the chain code it is never set)
    bool set = false, consumed = false;
    for (const size_t n : chains) {
        if (n > 0) set = true;
        else if (set) consumed = true;
    }
    SEQ_CHECK(set, "the Rally's chain to its held partners is set when its cast ends (the tree's next look drops it)");
    SEQ_CHECK(consumed, "... and is consumed at the next look, so nothing waits on the held partners");
    size_t early = 0;
    for (size_t i = 1; i < rally_times.size(); ++i) {
        if (rally_times[i] - rally_times[i - 1] < 40.0) ++early;
    }
    SEQ_CHECKF(early == 0, "the Rally is not repeated before the 40 s its own timer starts (%zu pairs closer than 40 s)", early);
}

AOC_REGISTER_TEST(test_goblin_rally_with_held_partners);

// R6: the columns of the table: the build's table carries the sequence, a table with no sequence column still loads (every table made before the sequences, and every test that
// writes one, keeps its behaviour), and the same tree with the column empty plays the same fight.  The Wolf's tree has no sequence in the build's table, so the three forms must play
// the same fight.
void test_no_sequence_column_changes_nothing(Fixture& f) {
    std::printf("a creature table with no sequence column loads as before, the build's table keeps its column, and the same tree with an empty column plays exactly the same fight\n");
    const std::string path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    SEQ_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Wolf");
    if (path.empty() || !f.motion.find(path)) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    combat::NpcAbilityData real;
    if (!load_creature_tables(real)) return;
    const std::vector<combat::CreatureAbility>* wolf = real.abilities_of(path);
    SEQ_CHECK(wolf != nullptr, "the creature tables list the Wolf's tree");
    if (!wolf) return;
    size_t linked = 0;
    for (const combat::CreatureAbility& t : *wolf) linked += t.next.empty() ? 0 : 1;
    SEQ_CHECKF(linked == 0, "the Wolf's tree in the build's table has no sequence, so the three forms must play the same fight (%zu linked rows)", linked);
    if (linked != 0) return;
    std::string error;
    const std::string kept = sequences::table_with_next(dir, path, {}, error, sequences::NextColumn::Keep);
    const std::string empty = sequences::table_with_next(dir, path, {}, error, sequences::NextColumn::Empty);
    const std::string none = sequences::table_with_next(dir, path, {}, error, sequences::NextColumn::None);
    SEQ_CHECKF(!kept.empty() && !empty.empty() && !none.empty(), "the Wolf's table is written with its own column, with an empty column and without it (%s)", error.c_str());
    if (kept.empty() || empty.empty() || none.empty()) return;
    sequences::SequenceFight a, b, c;
    if (!sequences::run_sequence_fight(f, kept, path, 30.0, 0, 500, a)) return;
    if (!sequences::run_sequence_fight(f, empty, path, 30.0, 0, 500, b)) return;
    if (!sequences::run_sequence_fight(f, none, path, 30.0, 0, 500, c)) return;
    const std::vector<sequences::Cast> ca = sequences::casts_of(a.samples, 1.0 / 15.0), cb = sequences::casts_of(b.samples, 1.0 / 15.0), cc = sequences::casts_of(c.samples, 1.0 / 15.0);
    const auto same = [](const std::vector<sequences::Cast>& x, const std::vector<sequences::Cast>& y) {
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); ++i) {
            if (x[i].ability != y[i].ability || std::fabs(x[i].t - y[i].t) >= 1e-9) return false;
        }
        return true;
    };
    SEQ_CHECKF(same(ca, cb) && same(ca, cc), "the same casts at the same moments with the column, with it empty and without it (%zu, %zu and %zu casts)", ca.size(), cb.size(), cc.size());
}

AOC_REGISTER_TEST(test_no_sequence_column_changes_nothing);

// R4: the Birdperson Stalker's Shift is an alert that starts the ShiftCooldown its own Lunge goes by.  The alert's own timer counts as a cooldown it readies, so the Shift is
// not played alone (it would keep the Lunge from being ready); the Lunge itself still plays.  Before the fix the Shift played, reset ShiftCooldown and starved the Lunge.
void test_the_birdpersons_shift_does_not_starve_its_lunge(Fixture& f) {
    std::printf("a Birdperson Stalker's Shift starts ShiftCooldown, the timer its Lunge goes by: the alert is not played alone and the Lunge still plays\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Humanoid/Birdperson/Birdperson_Stalker.Default__Birdperson_Stalker_C");
    const abilities::AbilityDef* shift = f.catalog->ability_named("Birdperson_ShiftLeft");
    const abilities::AbilityDef* lunge = f.catalog->ability_named("Birdperson_Lunge");
    SEQ_CHECK(!path.empty() && f.motion.find(path) && tables.abilities_of(path) && shift && lunge, "the Blueprint table has the Stalker and the ability data has its Shift and Lunge");
    if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path) || !shift || !lunge) return;
    bool named = false;
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        if (t.ability == lunge->row.guid && t.task == "ShiftCooldown") named = true;
    }
    SEQ_CHECK(named, "the table has a Lunge row whose own timer is ShiftCooldown (the cooldown the Shift starts)");
    if (!named) return;
    BanditOpen made;
    SEQ_CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the Stalker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    std::vector<LungeSample> samples;
    SEQ_CHECK(run_lunge_fight(f, tables, path, 60.0, 0, 800, nullptr, samples, nullptr, false, -1, nullptr, 40000.0, made.open), "a 60 s fight against a player who stands 8 m away runs");
    if (samples.empty()) return;
    const std::vector<sequences::Cast> casts = sequences::casts_of(samples, 1.0 / 15.0);
    size_t shifts = 0, lunges = 0;
    for (const sequences::Cast& c : casts) {
        shifts += c.ability == shift->row.guid ? 1 : 0;
        lunges += c.ability == lunge->row.guid ? 1 : 0;
    }
    std::printf("  %zu shifts and %zu lunges in 60 s\n", shifts, lunges);
    SEQ_CHECKF(shifts == 0, "the Shift is not played on its own: it starts the timer its Lunge goes by (%zu casts)", shifts);
    SEQ_CHECKF(lunges >= 1, "the Lunge is still played (%zu casts)", lunges);
}

AOC_REGISTER_TEST(test_the_birdpersons_shift_does_not_starve_its_lunge);

// R15 (the common cases): two mobs of the same kind side by side both play the chain; one that resets drops the chain its cast had readied and does not play it; an actor made
// again from the same Blueprint (a respawn) starts its own tree and plays the chain.  The Flayer's real table links its sneer to its spin.
void test_chains_with_two_mobs_a_reset_and_a_respawn(Fixture& f) {
    std::printf("two Flayers side by side each play their sneer and spin; the one that resets drops the pair its cast readied; a Flayer made again plays its own\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Humanoid/Goblin/GoblinSlaver/GoblinFlayer/Goblin_Flayer.Default__Goblin_Flayer_C");
    const abilities::AbilityDef* sneer = f.catalog->ability_named("GoblinFlayer_ManicSneer");
    const abilities::AbilityDef* spin = f.catalog->ability_named("GoblinFlayer_SeveringSpin");
    SEQ_CHECK(!path.empty() && f.motion.find(path) && tables.abilities_of(path) && sneer && spin, "the Blueprint table has the Flayer and its sneer and spin");
    if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path) || !sneer || !spin) return;
    const auto bp = f.motion.find(path);
    Rig rig(4, 20, {});
    combat_rig::World w;
    SEQ_CHECK(rig.ok() && w.problem.empty(), "the two mobs' fight's world is made");
    if (!rig.ok() || !w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    BanditOpen made;
    SEQ_CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the Flayers (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    const uint64_t a = w.spawn(path, -150, 0, 7, 900, bp->half_height, made.open, &*bp);
    const uint64_t b = w.spawn(path, 150, 0, 7, 900, bp->half_height, made.open, &*bp);
    SEQ_CHECK(a != 0 && b != 0, "the two Flayers are put in the world side by side");
    if (a == 0 || b == 0) return;
    combat_rig::FakePlayer player(rig, 77, {0, 900, 1098});
    world.add_player(&player);
    const double dt = 1.0 / 15.0;
    GremCastView last_a, last_b, last_c;
    std::vector<double> sneers_a, spins_a, sneers_b, spins_b, sneers_c, spins_c;
    uint64_t c = 0;
    double reset_at = -1;
    bool saw_b_sneer = false;
    for (double now = dt; now <= 105.0 + 1e-9; now += dt) {
        world.add_threat(a, player.actor_id(), 100.0, now);
        world.add_threat(b, player.actor_id(), 100.0, now);
        if (c == 0 && now >= 52.0) c = w.spawn(path, 0, -200, 7, 900, bp->half_height, made.open, &*bp);
        if (c != 0) world.add_threat(c, player.actor_id(), 100.0, now);
        if (const stats::StatValue* max = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max->value);
        w.npcs->note_player(player.actor_id(), 0, 900, now);
        w.npcs->tick(now);
        world.tick(now);
        const auto look = [&](uint64_t id, GremCastView& last, std::vector<double>& sneers, std::vector<double>& spins) {
            const GremCastView view = grem_cast_view(w.registry, id);
            if (view.montage != 0 && (view.element != last.element || last.montage == 0)) {
                if (view.ability == sneer->row.guid) sneers.push_back(now);
                if (view.ability == spin->row.guid) spins.push_back(now);
            }
            last = view;
        };
        look(a, last_a, sneers_a, spins_a);
        look(b, last_b, sneers_b, spins_b);
        if (c != 0) look(c, last_c, sneers_c, spins_c);
        // the reset mob: when its sneer has begun, it resets before the sneer's montage could hand the chain over (the pair is dropped)
        if (!saw_b_sneer && !sneers_b.empty()) {
            saw_b_sneer = true;
            reset_at = now;
            std::string why;
            world.gm_reset(b, now);
        }
    }
    std::printf("  a: %zu sneers, %zu spins; b (reset at %.1f s): %zu sneers, %zu spins; c (made again at 52 s): %zu sneers, %zu spins\n", sneers_a.size(), spins_a.size(),
                reset_at, sneers_b.size(), spins_b.size(), sneers_c.size(), spins_c.size());
    SEQ_CHECKF(!sneers_a.empty() && !spins_a.empty() && spins_a.front() >= sneers_a.front() && spins_a.front() - sneers_a.front() <= 4.0,
               "the first Flayer plays the pair (sneer %.1f s, spin %.1f s)", sneers_a.empty() ? -1.0 : sneers_a.front(), spins_a.empty() ? -1.0 : spins_a.front());
    SEQ_CHECK(saw_b_sneer && !sneers_b.empty(), "the second Flayer began its sneer and reset in it");
    const auto spin_after_reset = [&]() {
        for (const double t : spins_b) {
            if (t > reset_at && t < reset_at + 10.0) return true;
        }
        return false;
    };
    SEQ_CHECKF(!spin_after_reset(), "the reset dropped the pair its sneer had readied: no spin follows it (%zu spins after %.1f s)", spins_b.size(), reset_at);
    SEQ_CHECKF(!sneers_c.empty() && !spins_c.empty() && spins_c.front() - sneers_c.front() <= 4.0, "the Flayer made again plays its own pair (sneer %.1f s, spin %.1f s)",
               sneers_c.empty() ? -1.0 : sneers_c.front(), spins_c.empty() ? -1.0 : spins_c.front());
}

AOC_REGISTER_TEST(test_chains_with_two_mobs_a_reset_and_a_respawn);

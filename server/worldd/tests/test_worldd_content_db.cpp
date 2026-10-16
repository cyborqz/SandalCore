// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/test_worldd_content_db.cpp
//
//  worldd's read-only reader of content.db (server/worldd/content/content_db.h, plan E1).
//
//  Unit tests build a small SQLite database in a temp directory (content_db_fixture.h) with
//  the tables and columns the reader uses.  Their names are services/internal/content/schema.go's
//  (the one place contentc's DDL lives): meta(key, value), class(guid, name, archetype_id),
//  class_stat_base(class_guid, level, stat_guid, stat_name, value, rounding_ambiguous).  Only those
//  columns are created; the reader needs no others.
//
//  The real test runs when AOC_CONTENT_DB names a content.db built by contentc
//  (cd services && go run ./cmd/contentc -> dist/content/content.db) and checks the
//  Bard (archetype 17747) at level 1 gives the capture's Strength 24, Dexterity 24,
//  Constitution 30, Mentality 30 and Wisdom 36, and Intelligence 39, the one
//  attribute that differs from the capture's 60 (plan C1's open item), and that
//  its stat Guids are the ones the capture's StatsComponent names.  Without the
//  variable it prints "[skip]".
//
//  Plan B3 adds the stat program (stat_program, which stats_fixture.h takes from the shared corpus) and the items' stat data
//  (quality_curve, stat_block_stat, item_set, set_bonus, item_tag, equip_slot; the starting kits' real rows are in gear_fixture.h):
//  contentc version 4 is the oldest file the reader opens, with a message saying so; item_gear, set_bonus, equip_slot_names and
//  ContentGearCatalog read what contentc wrote, on the fixture and, with AOC_CONTENT_DB, on the real file.
// ============================================================================
#include "bootstrap/captured_setup.h"
#include "content/content_db.h"
#include "content_db_fixture.h"
#include "stats/stat_program.h"

#include <sqlite3.h>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace content = aoc::worldd::content;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
// (CoA) (C:Y:B)

// ---- a small content.db (content_db_fixture.h) --------------------------------------------------------------------------

using content_fixture::DbOptions;
using content_fixture::kBard;
using content_fixture::kStrength;
using content_fixture::kTank;

static void write_db(const fs::path& path, const DbOptions& o = {}) {
    if (!content_fixture::make_db(path, o)) g_fail++;
}

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static void test_open_refusals(const fs::path& dir) {
    std::printf("open: what is refused\n");
    std::string error;

    const fs::path missing = dir / "missing.db";
    auto r = content::ContentDb::open(missing.string(), error);
    CHECK(!r && contains(error, "not found") && contains(error, "contentc"), "a missing file is an error that says how to build one");
    CHECK(!fs::exists(missing), "...and open() did not create it");

    const fs::path empty = dir / "empty.db";
    { std::ofstream f(empty, std::ios::binary); }
    error.clear();
    r = content::ContentDb::open(empty.string(), error);
    CHECK(!r && !error.empty(), "an empty file is not a content.db");
    CHECK(fs::file_size(empty) == 0, "...and was left empty (nothing was written into it)");

    const fs::path text = dir / "text.db";
    { std::ofstream f(text, std::ios::binary); f << "this is not a SQLite database, it is a line of text that is longer than the header"; }
    error.clear();
    r = content::ContentDb::open(text.string(), error);
    CHECK(!r && !error.empty(), "a text file is not a content.db");

    error.clear();
    r = content::ContentDb::open(dir.string(), error);
    CHECK(!r && contains(error, "not found"), "a directory is not a content.db");

    DbOptions v2;
    v2.schema_version = "2";
    write_db(dir / "v2.db", v2);
    error.clear();
    r = content::ContentDb::open((dir / "v2.db").string(), error);
    CHECK(!r && contains(error, "schema_version") && contains(error, "'2'"), "another schema version is refused");

    DbOptions no_version;
    no_version.contentc_version = false;
    write_db(dir / "nover.db", no_version);
    error.clear();
    r = content::ContentDb::open((dir / "nover.db").string(), error);
    CHECK(!r && contains(error, "contentc.version"), "a file with no contentc.version is not contentc's");

    DbOptions no_sha;
    no_sha.source_sha = false;
    write_db(dir / "nosha.db", no_sha);
    error.clear();
    r = content::ContentDb::open((dir / "nosha.db").string(), error);
    CHECK(!r && contains(error, "source.dbc.sha256"), "a file with no source.dbc.sha256 is not contentc's");

    DbOptions no_class;
    no_class.class_table = false;
    write_db(dir / "noclass.db", no_class);
    error.clear();
    r = content::ContentDb::open((dir / "noclass.db").string(), error);
    CHECK(!r && contains(error, "class"), "a file without the class table fails at open, not at the first login");

    DbOptions no_stats;
    no_stats.stat_table = false;
    write_db(dir / "nostats.db", no_stats);
    error.clear();
    r = content::ContentDb::open((dir / "nostats.db").string(), error);
    CHECK(!r && contains(error, "class_stat_base"), "a file without class_stat_base fails at open");
}

static void test_queries(const fs::path& dir) {
    std::printf("queries on a small content.db\n");
    const fs::path path = dir / "content.db";
    write_db(path);
    const auto size_before = fs::file_size(path);
    const auto time_before = fs::last_write_time(path);
    {
        std::string error;
        auto db = content::ContentDb::open(path.string(), error);
        CHECK(db.has_value(), "a content.db of schema version 1 opens");
        if (!db) return;
        CHECK(db->meta().schema_version == 1 && db->meta().contentc_version == "15" &&
                  db->meta().source_dbc_sha256 == "a2a3b82715732603e37a5cffcd422691f4402133cfdd0f231c7eb31025114a23" &&
                  db->meta().built_at == "2026-10-01T05:28:49Z",
              "meta: schema version, contentc version, the source's sha256 and the build time");

        const auto bard = db->class_for_archetype(17747, error);
        CHECK(bard && bard->guid == kBard && bard->name == "Bard" && bard->archetype_id == 17747, "archetype 17747 is the Bard, Guid 108640158570105");
        const auto tank = db->class_for_archetype(17754, error);
        CHECK(tank && tank->guid == kTank && tank->name == "Tank", "archetype 17754 is the Tank");
        error.clear();
        const auto none = db->class_for_archetype(17000, error);
        CHECK(!none && contains(error, "17000"), "an archetype with no class: nullopt and a message naming it");
        error.clear();
        CHECK(!db->class_for_archetype(0, error) && !error.empty(), "archetype 0 (a record with none) is not a class");
// [Cyb x C | O | A]

        // plan B1: a class by its name (the name of an ECharacterClass)
        error.clear();
        const auto bard_by_name = db->class_for_name("Bard", error);
        CHECK(bard_by_name && bard_by_name->guid == kBard && bard_by_name->name == "Bard" && bard_by_name->archetype_id == 17747,
              "the class named Bard is archetype 17747, Guid 108640158570105");
        const auto tank_by_name = db->class_for_name("Tank", error);
        CHECK(tank_by_name && tank_by_name->guid == kTank && tank_by_name->archetype_id == 17754, "the class named Tank is archetype 17754");
        error.clear();
        CHECK(!db->class_for_name("Paladin", error) && contains(error, "Paladin"), "a class the file has no row for: nullopt and a message naming it");
        error.clear();
        CHECK(!db->class_for_name("bard", error) && !db->class_for_name("", error) && !db->class_for_name("Bard ", error), "names are exact: bard, empty and 'Bard ' are no class");
        error.clear();
        CHECK(!db->class_for_name(std::string("Bard\0x", 6), error), "a name with a NUL is no class");

        const auto l1 = db->primary_attributes(kBard, 1, error);
        CHECK(l1 && l1->size() == 6, "the Bard at level 1 has six attributes");
        if (l1 && l1->size() == 6) {
            bool ordered = true;
            for (size_t i = 1; i < l1->size(); ++i) ordered = ordered && (*l1)[i - 1].stat_guid < (*l1)[i].stat_guid;
            CHECK(ordered, "...ordered by stat Guid");
            std::map<std::string, double> by_name;
            bool ambiguous = false;
            for (const auto& a : *l1) {
                by_name[a.stat_name] = a.value;
                ambiguous = ambiguous || a.rounding_ambiguous;
            }
            CHECK(by_name["Strength"] == 24 && by_name["Dexterity"] == 24 && by_name["Constitution"] == 30 && by_name["Mentality"] == 30 &&
                      by_name["Wisdom"] == 36 && by_name["Intelligence"] == 39 && (*l1)[0].stat_guid == kStrength && !ambiguous,
                  "...Strength 24, Dexterity 24, Constitution 30, Mentality 30, Wisdom 36, Intelligence 39; none ambiguous");
        }
        const auto l2 = db->primary_attributes(kBard, 2, error);
        bool flagged = false;
        double strength = 0;
        if (l2) {
            for (const auto& a : *l2) {
                if (a.stat_guid == kStrength) {
                    flagged = a.rounding_ambiguous;
                    strength = a.value;
                }
            }
        }
        CHECK(l2 && flagged && strength == 26.5, "level 2: rounding_ambiguous comes through, with a fractional value as stored");
        const auto t1 = db->primary_attributes(kTank, 1, error);
        CHECK(t1 && t1->size() == 6 && (*t1)[0].stat_guid == kStrength && (*t1)[0].value == 40, "another class's rows are its own");

        error.clear();
        const auto l3 = db->primary_attributes(kBard, 3, error);
        CHECK(!l3 && contains(error, "level 3"), "a level with no rows: nullopt and a message naming the level");
        error.clear();
        CHECK(!db->primary_attributes(12345, 1, error) && !error.empty(), "an unknown class has no attributes");

        // a moved ContentDb is the same database
        content::ContentDb moved = std::move(*db);
        error.clear();
        CHECK(moved.class_for_archetype(17747, error).has_value(), "a moved ContentDb still answers");
    }
    CHECK(fs::file_size(path) == size_before && fs::last_write_time(path) == time_before, "the file was neither written nor touched by opening and querying it");
}

// ---- plan B3: the stat program and the items' stat data -----------------------------------------------------------------------

namespace sg = aoc::worldd::stats;

static void test_stat_content(const fs::path& dir) {
    std::printf("plan B3: contentc version 4, the stat program, the items' stat data\n");
    std::string error;

    // contentc before version 15 is refused, saying what to do (plan D1 raised it from 4, plan D2 from 5, plan E1 from 6, stage 2 from 7, stage 3 from 8, part 2 from 9, the mounts from 10, Nodes 1 from 11, Gathering stage 2 from 12, Gathering stage 3 from 13, "Foliage gathering" from 14)
    for (const char* old : {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "0"}) {
        DbOptions o;
        o.contentc_version_value = old;
        const fs::path p = dir / (std::string("cv") + old + ".db");
        write_db(p, o);
        error.clear();
        const auto r = content::ContentDb::open(p.string(), error);
        CHECK(!r && contains(error, (std::string("contentc version ") + old).c_str()) && contains(error, "reads version 15 or later") && contains(error, "go run ./cmd/contentc"),
              (std::string("a file built by contentc version ") + old + " does not open: the message says version 15 is needed and how to rebuild").c_str());
    }
    {
        DbOptions o;
        o.contentc_version_value = "four";
        write_db(dir / "cvfour.db", o);
        error.clear();
        CHECK(!content::ContentDb::open((dir / "cvfour.db").string(), error) && contains(error, "no number"), "a contentc.version that is no number is refused");
        o.contentc_version_value = "15";
        write_db(dir / "cv15.db", o);
        CHECK(content::ContentDb::open((dir / "cv15.db").string(), error).has_value(), "a later contentc version opens");
    }
    {
        DbOptions o;
        o.progress_tables = false;
        write_db(dir / "noprogress.db", o);
        error.clear();
        const auto r = content::ContentDb::open((dir / "noprogress.db").string(), error);
        CHECK(!r && contains(error, "does not have the tables this worldd reads"), "a file of version 6 without plan D1's tables fails at open");
    }
    {
        DbOptions o;
        o.stat_tables = false;
        write_db(dir / "nostat.db", o);
        error.clear();
        const auto r = content::ContentDb::open((dir / "nostat.db").string(), error);
        CHECK(!r && contains(error, "does not have the tables this worldd reads") && contains(error, "go run ./cmd/contentc"),
              "a file of version 6 without the stat tables fails at open, naming the rebuild");
    }
// C O A with Cyb

    // the stat program row
    const fs::path path = dir / "stat.db";
    write_db(path);
    auto db = content::ContentDb::open(path.string(), error);
    CHECK(db.has_value(), "the fixture opens");
    if (!db) return;
    const auto row = db->stat_program(error);
    CHECK(row.has_value(), "the stat program row is read");
    if (row) {
        const std::vector<uint8_t>& bundle = stats_fixture::main_bundle();
        const uint32_t crc = static_cast<uint32_t>(bundle[12]) | (static_cast<uint32_t>(bundle[13]) << 8) | (static_cast<uint32_t>(bundle[14]) << 16) |
                             (static_cast<uint32_t>(bundle[15]) << 24);
        sg::StatBundle b;
        std::string perr;
        CHECK(sg::parse_bundle(row->data, b, perr), "the bytes parse and verify");
        CHECK(row->version == 3 && row->mode == "float32" && row->stats == static_cast<int>(b.stats.size()) && row->classes == static_cast<int>(b.classes.size()) && row->crc32 == crc &&
                  row->data == bundle && b.classes.size() == 10,
              "version 3, float32, the bundle's stats and its 10 classes (8 players, 2 NPC profiles), the header's crc32 and the bundle's own bytes");
    }
    {
        DbOptions o;
        const std::vector<uint8_t> none;
        o.stat_program_bytes = &none;
        write_db(dir / "norow.db", o);
        auto empty = content::ContentDb::open((dir / "norow.db").string(), error);
        error.clear();
        CHECK(empty && !empty->stat_program(error) && contains(error, "no stat program"), "a stat_program table with no row: nullopt, and the message says so (PlayerWorld adds the way out, once)");
    }

    // the equip slot names
    const auto slots = db->equip_slot_names(error);
    CHECK(slots && slots->size() == 83 && slots->at(0) == "Head" && slots->at(2) == "Chest" && slots->at(15) == "Primary" && slots->at(16) == "Secondary" &&
              slots->at(23) == "ResourceBag_0",
          "the 83 EEquipSlot names: Head 0, Chest 2, Primary 15, Secondary 16, ResourceBag_0 23");

    // an item: the starter chest of the Bard's kit (real rows)
    constexpr int64_t kChest = 6064630214426886486ll;
    constexpr int64_t kMitigation = 109183607068294ll;
    constexpr int64_t kMaxDurability = 6064629444242636800ll;
    error.clear();
    const auto chest = db->item_gear(kChest, error);
    CHECK(chest.has_value() && chest->record == kChest && chest->has_curve && error.empty(), "the starter chest has its gear data");
    if (chest) {
        CHECK(chest->bands[0].x == 1 && chest->bands[0].y == 5 && chest->bands[2].x == 16 && chest->bands[2].y == 30 && chest->bands[6].x == 101 && chest->bands[6].y == 200,
              "its quality curve: Common 1-5, Rare 16-30, Artifact 101-200");
        CHECK(chest->stats.size() == 2 && chest->stats[0].stat == kMitigation && chest->stats[0].min[0] == 20 && chest->stats[0].max[5] == 20 && chest->stats[0].min[6] == 0 &&
                  chest->stats[1].stat == kMaxDurability && chest->stats[1].min[0] == 11 && chest->stats[1].max[4] == 13,
              "its stat block, by stat Guid: 20 of the mitigation rating at every rarity but Artifact, and the durability 11 to 14");
        CHECK(chest->tags.size() == 2 && chest->tags[0] == "Item.Gear.Armor.Light" && chest->tags[1] == "Slot.Chest" && chest->sets.empty(), "its tags, in the record's order, and no set");
    }
    error.clear();
    CHECK(!db->item_gear(42, error) && error.empty(), "an item that is none: nullopt, and no error (the file is fine)");
    const auto bag = db->item_gear(content_fixture::item::kPotion, error);
    CHECK(bag && !bag->has_curve && bag->stats.empty() && bag->tags.empty() && bag->sets.empty(), "an item with no curve, no stat block, no tags and no sets has none");
    const auto cape = db->item_gear(content_fixture::item::kGatherCape, error);
    CHECK(cape && !cape->has_curve && cape->stats.empty(), "a made-up item of the fixture without a stat block has no stat data either");
    // the made-up items the session tests give stat blocks (content_db_fixture.h): the heavy chest's Constitution and Mentality
    const auto heavy = db->item_gear(content_fixture::item::kHeavyChest, error);
    CHECK(heavy && heavy->has_curve && heavy->stats.size() == 2 && heavy->stats[0].stat == content_fixture::kConstitution && heavy->stats[1].stat == content_fixture::kMentality &&
              heavy->stats[0].min[0] == 100 && heavy->stats[0].max[6] == 100,
          "the made-up heavy chest: Constitution +100 and Mentality +100 on the default curve"); 

    // sets: written into a copy of the fixture
    {
        const fs::path sp = dir / "sets.db";
        write_db(sp);
        sqlite3* raw = nullptr;
        bool ok = sqlite3_open_v2(sp.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK;
        ok = ok && content_fixture::exec(raw, "INSERT INTO item_set VALUES (6064630214426886486, 0, 7001), (6064630214426886486, 1, 7002)");
        ok = ok && content_fixture::exec(raw, "INSERT INTO set_bonus (set_guid, tier_ord, pieces, bonus_ord, stat_guid, common, rare) VALUES "
                                              "(7001, 0, 2, 0, 109183598614138, 10, 30), (7001, 0, 2, 1, 109183598614139, NULL, NULL), "
                                              "(7001, 1, 4, 0, 109183598614138, 5, NULL), "
                                              // a negative tier_ord (a file this checkout's contentc never writes): refused, not tiers.back() of an empty vector
                                              "(7003, -1, 2, 0, 109183598614138, 10, NULL), (7003, 0, 4, 0, 109183598614138, 5, NULL), (7004, -2, 2, 0, 109183598614138, 7, NULL)");
        if (raw) sqlite3_close(raw);
        CHECK(ok, "the sets are written into a copy of the fixture");
        auto sdb = content::ContentDb::open(sp.string(), error);
        const auto with_sets = sdb ? sdb->item_gear(kChest, error) : std::nullopt;
        CHECK(with_sets && with_sets->sets.size() == 2 && with_sets->sets[0] == 7001 && with_sets->sets[1] == 7002, "the chest belongs to sets 7001 and 7002, in the record's order");
        const auto s1 = sdb ? sdb->set_bonus(7001, error) : std::nullopt;
        CHECK(s1 && s1->tiers.size() == 2 && s1->tiers[0].pieces == 2 && s1->tiers[1].pieces == 4 && s1->tiers[0].entries.size() == 2 && s1->tiers[1].entries.size() == 1,
              "set 7001: a tier of 2 pieces with two bonuses and a tier of 4 with one");
        if (s1 && s1->tiers.size() == 2) {
            const sg::SetEntry& e = s1->tiers[0].entries[0];
            CHECK(e.stat == 109183598614138ll && e.has[0] && e.value[0] == 10 && !e.has[1] && e.has[2] && e.value[2] == 30 && !e.has[6], "a bonus's value at Common 10 and Rare 30, none at the others");
            const sg::SetEntry& none = s1->tiers[0].entries[1];
            bool any = false;
            for (const bool h : none.has) any = any || h;
            CHECK(none.stat == 109183598614139ll && !any, "a bonus with NULL for every rarity is kept (it adds 0) with no value");
        }
        error.clear();
        CHECK(sdb && !sdb->set_bonus(7002, error) && error.empty(), "a set with no rows: nullopt and no error");
        for (const int64_t bad : {int64_t{7003}, int64_t{7004}}) {
            error.clear();
            const auto refused = sdb ? sdb->set_bonus(bad, error) : std::optional<sg::SetBonus>(sg::SetBonus{});
            CHECK(!refused && error.find("negative tier order") != std::string::npos,
                  "a set whose first row has a negative tier_ord is refused with an error and no tier (never a crash)");
        }
// -- C | O | A . Cyb --

        // the catalog on it
        content::ContentGearCatalog catalog(*sdb);
        CHECK(catalog.ok() && catalog.slot_name(2) == "Chest" && catalog.slot_name(15) == "Primary" && catalog.slot_name(9999) == "Slot9999",
              "the catalog names the slots (Chest, Primary) and gives Slot<N> to a value that has none");
        const sg::ItemGear* a = catalog.item(kChest);
        const sg::ItemGear* b = catalog.item(kChest);
        CHECK(a && a == b && a->sets.size() == 2, "an item is read once and kept: the same pointer twice");
        CHECK(catalog.item(42) == nullptr && catalog.set(7002) == nullptr && catalog.set(7001) != nullptr && catalog.set(7001) == catalog.set(7001), "an unknown item and an unknown set are null; a set is kept too");
    }
}

// ---- the real content.db --------------------------------------------------------------------------------------------------

// ---- the mounts (contentc version 11) -------------------------------------------------------------------------------------

/// A content.db with the four mount tables the way services/internal/content/mounts.go lays them out, and a few rows.
static void test_mount_content(const fs::path& dir) {
    std::printf("the mounts: contentc version 11's tables\n");
    std::string error;
    {
        // a file without the tables (contentc version 10's shape) opens, and mount_data says how to get them
        write_db(dir / "nomounts.db");
        auto db = content::ContentDb::open((dir / "nomounts.db").string(), error);
        CHECK(db.has_value(), "a file without the mount tables opens (they are read when asked for)");
        if (!db) return;
        error.clear();
        const auto r = db->mount_data(error);
        CHECK(!r && contains(error, "mount") && contains(error, "contentc") && contains(error, "rebuild"), "mount_data on it fails and says to rebuild with contentc");
    }
    const fs::path p = dir / "mounts.db";
    write_db(p);
    {
        sqlite3* raw = nullptr;
        CHECK(sqlite3_open_v2(p.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "the fixture reopens for writing");
        const char* sqls[] = {
            "CREATE TABLE mount (item_guid INTEGER PRIMARY KEY, item_name TEXT NOT NULL, kind TEXT NOT NULL, blueprint_path TEXT NOT NULL, blueprint TEXT NOT NULL, "
            "activation TEXT NOT NULL, cast_ability_guid INTEGER NOT NULL, level INTEGER NOT NULL, ground_pct INTEGER NOT NULL, flying_pct INTEGER NOT NULL, "
            "water_pct INTEGER NOT NULL, health_pct INTEGER NOT NULL, mule INTEGER NOT NULL, crate_capacity INTEGER NOT NULL, tradeable INTEGER NOT NULL, "
            "description TEXT NOT NULL)",
            "CREATE TABLE mount_stat (level INTEGER NOT NULL, stat_guid INTEGER NOT NULL, stat_name TEXT NOT NULL, value REAL NOT NULL, PRIMARY KEY (level, stat_guid)) WITHOUT ROWID",
            "CREATE TABLE mount_ability (ability_guid INTEGER PRIMARY KEY, name TEXT NOT NULL, role TEXT NOT NULL, usable_while_mounted INTEGER NOT NULL, auto_dismount INTEGER NOT NULL)",
            "CREATE TABLE mount_effect (effect_guid INTEGER PRIMARY KEY, name TEXT NOT NULL, kind TEXT NOT NULL, tier INTEGER NOT NULL, stat_guid INTEGER NOT NULL, "
            "value REAL NOT NULL, relative INTEGER NOT NULL, infinite INTEGER NOT NULL, duration REAL NOT NULL, force_dismount INTEGER NOT NULL)",
            "INSERT INTO mount VALUES (6064631121016127508, 'Mount_Grounded_TerrorBird_0', 'ground', "
            "'/Game/GameBlueprints/Mounts/Mount_Grounded_TerrorBird_0.Mount_Grounded_TerrorBird_0_C', 'Mount_Grounded_TerrorBird_0', 'SummonMount', 108436548376185, 20, "
            "165, 0, 0, 100, 0, 0, 1, 'Ground Mount'), "
            "(6064634325298708480, 'Mount_Flying_Dawnbreaker_1_Alpha', 'flying', "
            "'/Game/GameBlueprints/Mounts/Mount_Flying_Dawnbreaker_1.Mount_Flying_Dawnbreaker_1_C', 'Mount_Flying_Dawnbreaker_1', 'CastAbility', 108436548376185, 25, "
            "165, 215, 0, 500, 0, 0, 0, 'Flying Mount')",
            "INSERT INTO mount_stat VALUES (20, 108986356618873, 'MaxHealth', 3572), (25, 108986356618873, 'MaxHealth', 4875), (25, 109228544644729, 'MoveSpeedMult', 1)",
            "INSERT INTO mount_ability VALUES (108436548376185, 'Summon_Mount', 'summon', 1, 0), (6064631555335526877, 'MountAbility_Sprint', 'dash', 0, 0), "
            "(6064628889369509888, 'Mage_Blink', '', 1, 1)",
            "INSERT INTO mount_effect VALUES (6064632988601155680, 'Road_T3_Speed_Mount', 'road', 3, 109228544644729, 0.25, 0, 1, 0, 0), "
            "(6064632423133890383, 'MountAbility_Sprint', 'dash', -1, 109228544644729, 0.35, 1, 0, 5, 0), "
            "(6064633425582162133, 'Rogue_Caltrops_Spooked', 'force_dismount', -1, 0, 0, 0, 0, 10, 1)",
        };
        for (const char* sql : sqls) {
            char* err = nullptr;
            if (sqlite3_exec(raw, sql, nullptr, nullptr, &err) != SQLITE_OK) {
                std::printf("  [FAIL] test setup: %s\n", err ? err : "?");
                sqlite3_free(err);
                g_fail++;
            }
        }
        sqlite3_close(raw);
    }
    auto db = content::ContentDb::open(p.string(), error);
    CHECK(db.has_value(), "the file opens");
    if (!db) return;
    const auto m = db->mount_data(error);
    CHECK(m && m->mounts.size() == 2 && m->stats.size() == 3 && m->abilities.size() == 3 && m->effects.size() == 3, "mount_data reads the four tables: 2 mounts, 3 stat rows, 3 abilities, 3 effects");
    if (!m) return;
    const content::MountRow* bird = m->find(6064631121016127508ll);
    CHECK(bird && bird->item_name == "Mount_Grounded_TerrorBird_0" && bird->kind == "ground" && bird->blueprint == "Mount_Grounded_TerrorBird_0" &&
              bird->activation == "SummonMount" && bird->cast_ability == 108436548376185ll && bird->level == 20 && bird->ground_pct == 165 && bird->flying_pct == 0 &&
              bird->health_factor() == 1.0 && bird->tradeable && !bird->mule,
          "the terror bird: ground, Blueprint Mount_Grounded_TerrorBird_0, SummonMount, level 20, 165% ground speed, health x1");
    const content::MountRow* dawn = m->find(6064634325298708480ll);
    CHECK(dawn && dawn->kind == "flying" && dawn->activation == "CastAbility" && dawn->flying_pct == 215 && dawn->health_factor() == 5.0 && !dawn->tradeable,
          "the Dawnbreaker: flying, CastAbility, 215% flying speed, health x5");
    CHECK(m->find(1) == nullptr, "an item that is no mount is not found");
    CHECK(m->stat_at(25, 108986356618873ll) == 4875 && m->stat_at(20, 108986356618873ll) == 3572 && m->stat_at(25, 109228544644729ll) == 1 && m->stat_at(26, 108986356618873ll, -1) == -1,
          "stat_at: MaxHealth 4875 at owner level 25 and 3572 at 20; a level the file has none of answers the fallback");
    const content::MountAbilityRow* summon = m->ability_with_role("summon");
    CHECK(summon && summon->name == "Summon_Mount" && summon->usable_while_mounted && !summon->auto_dismount && m->ability_with_role("dash") && !m->ability_with_role("dash")->usable_while_mounted &&
              m->ability_with_role("dismount_elevated") == nullptr,
          "the abilities by role: Summon_Mount is usable mounted, the dash is not the rider's, a role the file has no row for is none");
    const content::MountAbilityRow* blink = m->ability(6064628889369509888ll);
    CHECK(blink && blink->auto_dismount && blink->usable_while_mounted && blink->role.empty() && m->ability(5) == nullptr, "Mage_Blink dismounts the rider first and is usable mounted");
    CHECK(m->road(3) && m->road(3)->value == 0.25 && m->road(3)->infinite && m->road(3)->stat == 109228544644729ll && m->road(2) == nullptr && m->dash() && m->dash()->relative &&
              m->dash()->value == 0.35 && m->dash()->duration == 5.0,
          "the road effect T3 adds 0.25 to MoveSpeedMult, forever; the dash adds 0.35 of it for 5 s");
}
// == CoA | C.Y.B ==


static void test_real_content_db() {
    std::printf("the real content.db (AOC_CONTENT_DB)\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (!env || !*env) {
        std::printf("  [skip] AOC_CONTENT_DB is not set (cd services && go run ./cmd/contentc; AOC_CONTENT_DB=dist\\content\\content.db)\n");
        return;
    }
    std::string error;
    auto db = content::ContentDb::open(env, error);
    CHECK(db.has_value(), "the real content.db opens");
    if (!db) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    std::printf("    contentc version %s, built %s from the design database sha256 %s\n", db->meta().contentc_version.c_str(),
                db->meta().built_at.c_str(), db->meta().source_dbc_sha256.c_str());

    // the eight classes, archetypes 17747-17754 (services/internal/aocchar)
    const char* names[] = {"Bard", "Cleric", "Fighter", "Mage", "Ranger", "Rogue", "Summoner", "Tank"};
    bool classes_ok = true;
    std::map<std::string, int64_t> guids;
    for (int i = 0; i < 8; ++i) {
        const auto c = db->class_for_archetype(17747 + i, error);
        classes_ok = classes_ok && c && c->name == names[i];
        if (c) guids[c->name] = c->guid;
    }
    CHECK(classes_ok && guids.size() == 8, "archetypes 17747 to 17754 are Bard, Cleric, Fighter, Mage, Ranger, Rogue, Summoner, Tank");
    bool by_name = true;
    for (int i = 0; i < 8; ++i) {
        const auto c = db->class_for_name(names[i], error);
        by_name = by_name && c && c->guid == guids[names[i]] && c->archetype_id == 17747 + i;
    }
    CHECK(by_name, "class_for_name finds each of the eight by the name of its ECharacterClass, with the same Guid and archetype");
    CHECK(guids["Bard"] == kBard && guids["Tank"] == kTank, "the Bard's Guid is 108640158570105, as in the capture");

    // the Bard at level 1: the capture's values (Intelligence differs: plan C1's open item)
    const auto bard = db->class_for_archetype(17747, error);
    const auto l1 = bard ? db->primary_attributes(bard->guid, 1, error) : std::nullopt;
    CHECK(l1 && l1->size() == 6, "the Bard at level 1: six attributes");
    if (l1 && l1->size() == 6) {
        std::map<std::string, double> v;
        for (const auto& a : *l1) v[a.stat_name] = a.value;
        CHECK(v["Strength"] == 24 && v["Dexterity"] == 24 && v["Constitution"] == 30 && v["Mentality"] == 30 && v["Wisdom"] == 36,
              "Strength 24, Dexterity 24, Constitution 30, Mentality 30, Wisdom 36: the capture's values");
        CHECK(v["Intelligence"] == 39, "Intelligence 39 (the capture has 60: the known difference, plan C1)");

        // the stat Guids are the record Guids the capture's StatsComponent names for these six stats
        const auto stats = aoc::worldd::bootstrap::captured_bard_level1_stats();
        std::map<int64_t, uint32_t> captured_value;
        for (const auto& it : stats.owner.items) captured_value[it.record] = it.value;
        bool guids_found = true;
        for (const auto& a : *l1) guids_found = guids_found && captured_value.count(a.stat_guid) == 1;
        CHECK(guids_found, "the six stat Guids are items of the captured Owner stats");
        const auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
        bool same_but_int = true;
        for (const auto& a : *l1) {
            const bool is_int = a.stat_name == "Intelligence";
            const bool same = captured_value[a.stat_guid] == bits(static_cast<float>(a.value));
            same_but_int = same_but_int && (is_int ? !same && captured_value[a.stat_guid] == bits(60.0f) : same);
        }
        CHECK(same_but_int, "the captured Owner values equal content.db's as float bits, but Intelligence (captured 60.0)");
    }
// C.Y.B + CoA

    // every class and level 1..50 has six attributes; 7 of the 2400 rows are rounding-ambiguous
    size_t rows = 0, ambiguous = 0;
    bool complete = true;
    for (const auto& [name, guid] : guids) {
        for (int level = content::kMinLevel; level <= content::kMaxLevel; ++level) {
            const auto a = db->primary_attributes(guid, level, error);
            if (!a || a->size() != 6) {
                complete = false;
                continue;
            }
            for (const auto& x : *a) {
                ++rows;
                ambiguous += x.rounding_ambiguous ? 1 : 0;
            }
        }
    }
    CHECK(complete && rows == 2400, "8 classes x 50 levels x 6 attributes = 2400 rows");
    {
        // plan B3 on the real file: the stat program, the equip slot names and the starter chest's data
        const auto row = db->stat_program(error);
        sg::StatBundle b;
        std::string perr;
        CHECK(row && sg::parse_bundle(row->data, b, perr), "it parses and verifies");
        CHECK(row && row->version == 3 && row->mode == "float32" && row->stats == static_cast<int>(b.stats.size()) && row->classes == 10 && row->data.size() > 50205,
              "the stat program: format version 3 (plan E1, stage 3), float32, the bundle's stats, 10 classes (8 players, 2 NPC profiles), the stat graph and the ability expressions");
        CHECK(row && row->data == stats_fixture::main_bundle(), "it is the corpus's bundle, byte for byte");
        const auto slots = db->equip_slot_names(error);
        CHECK(slots && slots->size() == 83 && slots->at(2) == "Chest" && slots->at(82) == "None", "83 equip slot names");
        const auto chest = db->item_gear(6064630214426886486ll, error);
        CHECK(chest && chest->has_curve && chest->stats.size() == 2 && chest->tags.size() == 2, "the starter chest: a curve, two stats and two tags");
    }
    {
        // the mounts milestone on the real file: contentc version 11's tables
        std::string merror;
        const auto md = db->mount_data(merror);
        CHECK(md && md->mounts.size() == 101 && md->abilities.size() == 34 && md->effects.size() == 9 && md->stats.size() == 250,
              "the real file's mount tables: 101 mounts, 34 abilities, 9 effects, 250 stat rows (5 stats x 50 owner levels)");
        if (md) {
            size_t ground = 0, flying = 0, aquatic = 0, no_ground_text = 0;
            for (const auto& r : md->mounts) {
                ground += r.kind == "ground";
                flying += r.kind == "flying";
                aquatic += r.kind == "aquatic";
                no_ground_text += r.kind == "ground" && r.ground_pct == 0;
            }
            CHECK(ground == 51 && flying == 46 && aquatic == 4 && no_ground_text == 22, "51 ground, 46 flying, 4 aquatic; 22 ground items name no speed");
            const auto* bird = md->find(6064631121016127508ll);
            CHECK(bird && bird->ground_pct == 165 && bird->blueprint == "Mount_Grounded_TerrorBird_0", "the terror bird is 165% ground speed (capture 959: 990 cm/s)");
            const double hp25 = md->stat_at(25, 108986356618873ll);
            CHECK(hp25 == 4875 && md->stat_at(10, 108986356618873ll) == 1179 && md->stat_at(20, 108986356618873ll) == 3572, "Mount_Base MaxHealth at owner level 10, 20 and 25: 1179, 3572, 4875");
            const auto* dawn = md->find(6064634325298708480ll);
            CHECK(dawn && hp25 * dawn->health_factor() == 24375, "a Dawnbreaker of a level 25 owner has 24375 (4875 x 5: capture 871)");
            CHECK(md->road(0) && md->road(0)->value == 0.10 && md->road(4) && md->road(4)->value == 0.30 && md->dash() && md->dash()->value == 0.35 && md->dash()->duration == 5.0,
                  "the road effects add 0.10 to 0.30 to MoveSpeedMult; the dash adds 0.35 of it for 5 s");
            size_t usable = 0, auto_dismount = 0;
            for (const auto& a : md->abilities) {
                usable += a.usable_while_mounted;
                auto_dismount += a.auto_dismount;
            }
            CHECK(usable == 33 && auto_dismount == 12 && md->ability_with_role("summon") && md->ability_with_role("dash") && md->ability_with_role("dismount_elevated") &&
                      md->ability_with_role("dismount_grounded"),
                  "33 abilities are usable mounted and 12 dismount the rider first; Summon_Mount, the dash and the two dismounts have their roles");
        } else {
            std::printf("    %s\n", merror.c_str());
        }
    }
    CHECK(ambiguous == 7, "7 of them are rounding-ambiguous (services/README.md)");
    error.clear();
    CHECK(!db->primary_attributes(kBard, 51, error) && !error.empty(), "level 51 has none");
}

int main() {
    const fs::path dir = content_fixture::make_temp_dir("aoc_worldd_content_db_test_");
    test_open_refusals(dir);
    test_queries(dir);
    test_stat_content(dir);
    test_mount_content(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    test_real_content_db();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

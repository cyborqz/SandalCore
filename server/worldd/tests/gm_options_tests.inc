// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/gm_options_tests.inc  (included by test_worldd_gm_travel.cpp)
//
//  The values a GM command's field can take (WorldAdmin ListFieldOptions, GmTravel::field_options in session/gm_options.cpp): a form draws a drop-down from them instead of a box
//  the operator has to know the answer to.  The checks read the real content.db's data: the items (searched by text and paged), the rarities one item can show, the city nodes, the
//  free plots of a node, the weapon lines, the mounts, the quests, the recipes and the creatures; the lists that follow a character or another field (the quests a character has taken,
//  their active objectives, the plots of a node) follow what is chosen; and every option's value is one the run call takes.
// ============================================================================
#include "items/rarity.h"
#include "spawns/node_population.h"
#include "world/node_wire.h"

static GmOptions options_of(GmTravel& t, const std::string& command, const std::string& field, const std::map<std::string, std::string>& chosen = {}, const std::string& filter = {},
                            size_t limit = 0, size_t offset = 0) {
    return t.field_options(command, field, chosen, filter, limit, offset);
}

static const GmFieldOption* option_with_value(const GmOptions& o, const std::string& value) {
    for (const GmFieldOption& f : o.options) {
        if (f.value == value) return &f;
    }
    return nullptr;
}

/// The value and the label of the `i`th option, or "" when the list is shorter (a check on a list that is not there fails, it does not read past its end).
static std::string value_at(const GmOptions& o, size_t i) { return i < o.options.size() ? o.options[i].value : std::string(); }
static std::string label_at(const GmOptions& o, size_t i) { return i < o.options.size() ? o.options[i].label : std::string(); }

static bool labels_hold(const GmOptions& o, const std::string& text) {
    for (const GmFieldOption& f : o.options) {
        std::string l = f.label, w = text;
        for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (l.find(w) == std::string::npos) return false;
    }
    return true;
}

/// One whole number out of the real content.db, read by SQL on its own (the checks' reference, never the code under test).
static int64_t sql_number(const char* sql) {
    sqlite3* raw = nullptr;
    int64_t value = -1;
    if (sqlite3_open_v2(real_db_path().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        return -1;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) value = sqlite3_column_int64(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(raw);
    return value;
}

/// A world over the real content.db with the data the lists read: the recipes, the quests, the mounts, the city nodes (Joeva a Town, Halcyon a Village, Winstead a Camp, the rest
/// Wilderness) and their plots.  No player is in it; the creature lists add their own actors.
struct ListsWorld {
    Fixture                            fx;
    std::unique_ptr<world::CityNodes>  nodes;
    std::unique_ptr<world::NodeWire>   wire;
    std::unique_ptr<PlayerWorld>       world;
    std::unique_ptr<GmTravel>          travel;
    bool                               ok = false;

    ListsWorld() {
        std::string error;
        nodes = world::CityNodes::load(real_db_path(), error);
        if (!nodes) {
            std::printf("  [FAIL] the city nodes do not load: %s\n", error.c_str());
            return;
        }
        const world::CityNodeInfo* joeva = nodes->by_name("Joeva");
        const world::CityNodeInfo* halcyon = nodes->by_name("Halcyon");
        const world::CityNodeInfo* winstead = nodes->by_name("Winstead");
        if (!joeva || !halcyon || !winstead) {
            std::printf("  [FAIL] the real data has no Joeva, Halcyon or Winstead\n");
            return;
        }
        nodes->apply_snapshot({{joeva->guid, 4, 0, 0}, {halcyon->guid, 3, 0, 0}, {winstead->guid, 1, 0, 0}});
        wire = world::NodeWire::load(real_db_path(), *nodes, error);
        if (!wire) {
            std::printf("  [FAIL] the node wire does not load: %s\n", error.c_str());
            return;
        }
        world = std::make_unique<PlayerWorld>(&*fx.real, "", bs::GameStateMode::Tier2, kServerId, fx.guids);
        world->set_city_nodes(nodes.get());
        world->set_node_wire(wire.get());
        world->load_crafting_data();
        auto quests = fx.real->quest_data(error);
        if (!quests) {
            std::printf("  [FAIL] the quests do not load: %s\n", error.c_str());
            return;
        }
        world->set_quests(qs::QuestBook(std::move(*quests)));
        auto book = spawns::MountBook::load(*fx.real, error);
        if (!book) {
            std::printf("  [FAIL] the mounts do not load: %s\n", error.c_str());
            return;
        }
        world->set_mounts(std::move(*book));
        travel = std::make_unique<GmTravel>(*world, [] { return std::vector<PlayerHandle>{}; }, [] { return Clock::now(); });
        ok = true;
    }
};

/// Every field the list says has options, by "command.field": the checks below hold the catalog to exactly these.
static const std::vector<std::string>& fields_with_options() {
    static const std::vector<std::string> names = {
        "teleport.place",        "give-item.item",         "give-item.rarity",     "place-station.node",       "place-station.plot",      "place-station.station",
        "remove-station.node",   "remove-station.plot",    "finish-jobs.station",  "teach-recipe.recipe",      "forget-recipe.recipe",    "weapon-xp.line",
        "npc-kill.id",           "npc-respawn.id",         "npc-reset.id",         "npc-heal.id",              "npc-despawn.id",          "npc-move.id",
        "start-quest.quest",     "complete-quest.quest",   "complete-objective.quest", "complete-objective.objective", "give-mount.mount", "node-xp.node"};
    return names;
}

// ---- the catalog --------------------------------------------------------------------------------------------------------------------------------------------------

static void test_gm_options_catalog() {
    std::printf("the field options: the list says which fields have them, what they follow and which are long\n");
    std::set<std::string> found;
    bool depends_ok = true, searchable_ok = true, character_ok = true, free_numbers_ok = true;
    for (const gma::AdminCommand& c : gma::admin_commands()) {
        for (const gma::CommandField& f : c.fields) {
            if (f.has_options) found.insert(c.name + "." + f.name);
            for (const std::string& d : f.depends_on) {
                bool is_field = false;
                for (const gma::CommandField& other : c.fields) is_field = is_field || (other.name == d && other.name != f.name);
                depends_ok = depends_ok && is_field && f.has_options;
            }
            searchable_ok = searchable_ok && (f.searchable == (f.page_size > 0)) && (!f.searchable || f.has_options);
            character_ok = character_ok && !(f.type == gma::FieldType::Character && f.has_options);
            // a free number keeps its box: no whole number or number field has options, except the ids and Guids that name a thing in the world
            if ((f.type == gma::FieldType::WholeNumber || f.type == gma::FieldType::Number) && f.has_options) {
                const bool names_a_thing = f.name == "id" || f.name == "objective";
                free_numbers_ok = free_numbers_ok && names_a_thing;
            }
        }
    }
    const std::set<std::string> expected(fields_with_options().begin(), fields_with_options().end());
    CHECK(found == expected, "exactly the fields that name a thing of the world have options (24 of them), no other");
    CHECK(depends_ok, "every field a list follows is another field of the same command");
    CHECK(searchable_ok, "a searchable field has a page size, and only a field with options is searchable");
    CHECK(character_ok, "the character field is the player picker, not an options field");
    CHECK(free_numbers_ok, "a free number (a count, an amount, a point, a level) has no options: it stays a box with its range");
    const gma::AdminCommand* station = gma::find_command("place-station");
    const gma::CommandField* plot = nullptr;
    const gma::CommandField* st = nullptr;
    for (const gma::CommandField& f : station->fields) {
        if (f.name == "plot") plot = &f;
        if (f.name == "station") st = &f;
    }
    CHECK(plot && plot->depends_on == std::vector<std::string>{"node"} && st && st->depends_on == std::vector<std::string>({"node", "plot"}),
          "place-station: the plots follow the node, the stations follow the node and the plot");
    const gma::AdminCommand* give = gma::find_command("give-item");
    bool item_searchable = false, rarity_follows_item = false;
    for (const gma::CommandField& f : give->fields) {
        if (f.name == "item") item_searchable = f.searchable && f.page_size == gma::kOptionsPageSize;
        if (f.name == "rarity") rarity_follows_item = f.has_options && f.depends_on == std::vector<std::string>{"item"};
    }
    CHECK(item_searchable && rarity_follows_item, "give-item: the items are searchable (a page of 50), the rarities follow the item");
    const gma::AdminCommand* objective = gma::find_command("complete-objective");
    const gma::CommandField* quest_field = nullptr;
    const gma::CommandField* objective_field = nullptr;
    for (const gma::CommandField& f : objective->fields) {
        if (f.name == "quest") quest_field = &f;
        if (f.name == "objective") objective_field = &f;
    }
    CHECK(quest_field && !quest_field->required && quest_field->depends_on == std::vector<std::string>{"character"} && objective_field &&
              objective_field->depends_on == std::vector<std::string>({"character", "quest"}),
          "complete-objective: an optional quest of the character's, and the objectives follow the character and the quest");
}
AOC_REGISTER_TEST(test_gm_options_catalog);

static void test_gm_options_every_field_is_served() {
    std::printf("the field options: every field that has options is answered, and the rest are refused\n");
    ListsWorld w;
    CHECK(w.ok, "(the lists' world is made)");
    if (!w.ok) return;
    bool all_served = true;
    for (const std::string& key : fields_with_options()) {
        const size_t dot = key.find('.');
        const GmOptions o = options_of(*w.travel, key.substr(0, dot), key.substr(dot + 1));
        if (o.status != GmStatus::Ok) std::printf("  [info] %s: %s\n", key.c_str(), o.message.c_str());
        all_served = all_served && o.status == GmStatus::Ok;
    }
    CHECK(all_served, "every field the list marks has a source in the world (no field is listed and then refused)");
    CHECK(options_of(*w.travel, "set-health", "value").status == GmStatus::InvalidArgument, "a free number (set-health's value) has no options: InvalidArgument");
    CHECK(options_of(*w.travel, "heal", "character").status == GmStatus::InvalidArgument, "the character field has none either (the page's own picker)");
    CHECK(options_of(*w.travel, "no-such-command", "x").status == GmStatus::InvalidArgument, "an unknown command is InvalidArgument");
    const GmOptions nofield = options_of(*w.travel, "give-item", "colour");
    CHECK(nofield.status == GmStatus::InvalidArgument && nofield.message.find("no field") != std::string::npos, "an unknown field is InvalidArgument, and the answer says so");
    CHECK(options_of(*w.travel, "GIVE-ITEM", "item").status == GmStatus::Ok, "a command's name is matched without case, as the run call does");
}
AOC_REGISTER_TEST(test_gm_options_every_field_is_served);

// ---- the items ----------------------------------------------------------------------------------------------------------------------------------------------------

static void test_gm_options_items() {
    std::printf("the items: searched by text, paged, every value an item the run call takes\n");
    ListsWorld w;
    if (!w.ok) return;
    const int64_t all_items = sql_number("SELECT COUNT(*) FROM item");
    CHECK(all_items > 1000, "(the real data has thousands of items)");
    const GmOptions first = options_of(*w.travel, "give-item", "item");
    CHECK(first.status == GmStatus::Ok && first.options.size() == 50 && static_cast<int64_t>(first.total) == all_items && first.more,
          "with no filter the first page is 50 items, the total is every item, and there is more");
    const GmOptions second = options_of(*w.travel, "give-item", "item", {}, {}, 0, 50);
    CHECK(second.options.size() == 50 && value_at(second, 0) != value_at(first, 0) && option_with_value(first, value_at(second, 0)) == nullptr, "the next page starts after the first (no item twice)");
    const GmOptions big = options_of(*w.travel, "give-item", "item", {}, {}, 100000, 0);
    CHECK(big.options.size() == gma::kOptionsMaxPage, "a page larger than the server's cap is cut to the cap (200)");
    const GmOptions three = options_of(*w.travel, "give-item", "item", {}, {}, 3, 0);
    CHECK(three.options.size() == 3 && three.more, "a limit of 3 gives 3");
    bool known = true, in_order = true;
    std::string previous;
    for (const GmFieldOption& o : first.options) {
        std::string error;
        const auto rec = w.fx.real->item(std::strtoll(o.value.c_str(), nullptr, 10), error);
        known = known && rec.has_value() && !o.label.empty();
        std::string l = o.label;
        for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        in_order = in_order && (previous.empty() || previous <= l);
        previous = l;
    }
    CHECK(known, "every value is the Guid of an item of the data, and has a label");
    CHECK(in_order, "the items are in the order of what the player reads");
    const GmOptions dawn = options_of(*w.travel, "give-item", "item", {}, "dawnbreaker");
    CHECK(dawn.status == GmStatus::Ok && !dawn.options.empty() && dawn.total == dawn.options.size() && !dawn.more && labels_hold(dawn, "dawnbreaker"),
          "a filter keeps the items whose name holds the text (case does not count)");
    const GmOptions none = options_of(*w.travel, "give-item", "item", {}, "zzzz-no-such-item");
    CHECK(none.options.empty() && none.total == 0 && !none.note.empty(), "a filter that matches nothing answers an empty list and says so");
    const GmOptions percent = options_of(*w.travel, "give-item", "item", {}, "%");
    CHECK(static_cast<int64_t>(percent.total) < all_items, "a % in the filter is a letter, not a wildcard");
    const GmOptions by_guid = options_of(*w.travel, "give-item", "item", {}, value_at(first, 0));
    CHECK(!first.options.empty() && option_with_value(by_guid, value_at(first, 0)) != nullptr, "an item is also found by its Guid");
}
AOC_REGISTER_TEST(test_gm_options_items);

static void test_gm_options_rarities() {
    std::printf("the rarities: Common to Artifact, narrowed to what the chosen item can show\n");
    ListsWorld w;
    if (!w.ok) return;
    const GmOptions all = options_of(*w.travel, "give-item", "rarity");
    CHECK(all.status == GmStatus::Ok && all.options.size() == 7 && value_at(all, 0) == "Common" && value_at(all, 6) == "Artifact", "before an item is chosen: the seven rarities, Common to Artifact");
    CHECK(label_at(all, 2) == "Rare (quality 16 to 30)" && label_at(all, 6) == "Artifact (quality 101 to 200)", "... each with its quality band");
    const int64_t narrow = sql_number("SELECT v.item_guid FROM item_vendor v JOIN item i ON i.guid = v.item_guid WHERE v.rarity_min = 2 AND v.rarity_max = 5 AND i.default_quality >= 1 "
                                      "AND i.quality_curve_guid IS NOT NULL AND i.quality_curve_guid <> 0 ORDER BY v.item_guid LIMIT 1");
    CHECK(narrow > 0, "(the data has an item that is Rare to Legendary)");
    if (narrow > 0) {
        const GmOptions o = options_of(*w.travel, "give-item", "rarity", {{"item", std::to_string(narrow)}});
        std::string names;
        for (const GmFieldOption& f : o.options) names += f.value + " ";
        CHECK(o.status == GmStatus::Ok && names == "Rare Heroic Epic Legendary ", "an item that is Rare to Legendary offers exactly Rare, Heroic, Epic and Legendary");
        std::string error;
        const auto rec = w.fx.real->item(narrow, error);
        bool accepted = rec.has_value();
        for (const GmFieldOption& f : o.options) {
            std::string why;
            accepted = accepted && rec && items::quality_for_request(*rec, rec->name, f.value, 0, why) != 0;
        }
        CHECK(accepted, "every rarity offered is one the run call accepts for that item");
        bool refused = rec.has_value();
        std::string why;
        refused = refused && rec && items::quality_for_request(*rec, rec->name, "Common", 0, why) == 0;
        CHECK(refused && !option_with_value(o, "Common"), "... and Common, which the item can never show, is not offered");
    }
    const int64_t plain = sql_number("SELECT guid FROM item WHERE default_quality < 1 ORDER BY guid LIMIT 1");
    CHECK(plain > 0, "(the data has an item with no quality)");
    if (plain > 0) {
        const GmOptions o = options_of(*w.travel, "give-item", "rarity", {{"item", std::to_string(plain)}});
        CHECK(o.status == GmStatus::Ok && o.options.empty() && o.note.find("no quality") != std::string::npos, "an item with no quality offers no rarity, and the note says why");
    }
    const GmOptions unknown = options_of(*w.travel, "give-item", "rarity", {{"item", "No_Such_Item_Record"}});
    CHECK(unknown.status == GmStatus::Ok && unknown.options.empty() && unknown.note.find("No item is named") != std::string::npos, "an item that is no item offers none, and the note says so");
}
AOC_REGISTER_TEST(test_gm_options_rarities);

// ---- the places, the city nodes and their plots ---------------------------------------------------------------------------------------------------------------------

static void test_gm_options_nodes_and_plots() {
    std::printf("the places, the city nodes and the plots of a node\n");
    ListsWorld w;
    if (!w.ok) return;
    const GmOptions places = options_of(*w.travel, "teleport", "place");
    bool login = false;
    for (const GmFieldOption& o : places.options) login = login || o.value == "login";
    CHECK(places.status == GmStatus::Ok && login && option_with_value(places, "Verra_RVR_Joeva") != nullptr && places.options.size() == w.world->places().all().size(),
          "teleport's places: every place the world takes by name, the login spot and the city nodes");
    const GmOptions all_nodes = options_of(*w.travel, "node-xp", "node");
    size_t with_place = 0;
    for (const world::CityNodeInfo& n : w.nodes->nodes()) with_place += world::has_travel_place(n) ? 1 : 0;
    CHECK(all_nodes.options.size() == with_place && with_place > 20 && option_with_value(all_nodes, "Verra_RVR_Joeva") != nullptr, "node-xp's nodes: every city node that is on the map");
    const GmOptions towns = options_of(*w.travel, "place-station", "node");
    CHECK(towns.options.size() == 2 && option_with_value(towns, "Verra_RVR_Joeva") && option_with_value(towns, "Verra_RVR_Halcyon") && !option_with_value(towns, "Verra_RVR_Winstead"),
          "place-station's nodes: only the Village and the Town (a Camp and the Wilderness have no service plots)");
    const GmFieldOption* joeva = option_with_value(towns, "Verra_RVR_Joeva");
    CHECK(joeva && joeva->label == std::string("Joeva (") + spawns::node_level_name(4) + ")", "... each labelled with its name and its level");
    CHECK(options_of(*w.travel, "remove-station", "node").options.empty() && options_of(*w.travel, "remove-station", "node").note == "No station was placed.",
          "remove-station's nodes: none while no station is placed, and the note says so");

    const GmOptions plots = options_of(*w.travel, "place-station", "plot", {{"node", "Verra_RVR_Joeva"}});
    CHECK(plots.status == GmStatus::Ok && !plots.options.empty(), "the free service plots of the Town");
    std::string why;
    const std::vector<PlayerWorld::StationPlot> direct = w.world->station_plots("Joeva", false, why);
    CHECK(direct.size() == plots.options.size() && plots.options.size() == plots.total, "... the same plots the place call looks at (the node is found by any of its names)");
    const GmOptions by_display = options_of(*w.travel, "place-station", "plot", {{"node", "Joeva"}});
    CHECK(by_display.options.size() == plots.options.size(), "(the node's display name gives the same plots as its record name)");
    const GmOptions other = options_of(*w.travel, "place-station", "plot", {{"node", "Verra_RVR_Halcyon"}});
    CHECK(other.status == GmStatus::Ok && !other.options.empty(), "the plots follow the node: the Village has its own list");
    const GmOptions nonode = options_of(*w.travel, "place-station", "plot");
    CHECK(nonode.options.empty() && nonode.note == "Choose the node first.", "no node chosen: no plots, and the note says to choose the node");
    const GmOptions camp = options_of(*w.travel, "place-station", "plot", {{"node", "Verra_RVR_Winstead"}});
    CHECK(camp.options.empty() && camp.note.find("only a Village or a Town") != std::string::npos, "a Camp has no plots: the note says only a Village or a Town has them");
    const GmOptions lost = options_of(*w.travel, "place-station", "plot", {{"node", "Atlantis"}});
    CHECK(lost.options.empty() && lost.note.find("no city node is named") != std::string::npos, "a node that is none: no plots and a note");
    const GmOptions placed = options_of(*w.travel, "remove-station", "plot", {{"node", "Verra_RVR_Joeva"}});
    CHECK(placed.options.empty() && placed.note.find("No station was placed") != std::string::npos, "remove-station's plots: none while nothing is placed on the node");
    const GmOptions stations = options_of(*w.travel, "place-station", "station", {{"node", "Verra_RVR_Joeva"}});
    CHECK(stations.status == GmStatus::Ok && stations.options.empty() && !stations.note.empty(), "the stations of a plot: a world with no town people says so, and answers no option");
}
AOC_REGISTER_TEST(test_gm_options_nodes_and_plots);

// ---- the lists of the world's data --------------------------------------------------------------------------------------------------------------------------------

static void test_gm_options_world_lists() {
    std::printf("the weapon lines, the mounts, the quests, the recipes, the station definitions and the creatures\n");
    ListsWorld w;
    if (!w.ok) return;
    const GmOptions lines = options_of(*w.travel, "weapon-xp", "line");
    CHECK(w.world->weapon_lines() && lines.options.size() == w.world->weapon_lines()->size() && lines.options.size() > 10 && option_with_value(lines, "Weapon_Ranged_Wand") != nullptr,
          "weapon-xp's lines: every weapon line of the data, by the name the command takes");
    CHECK(options_of(*w.travel, "weapon-xp", "line").total == lines.options.size(), "(the total is the whole list: it is not searchable)");

    const GmOptions mounts = options_of(*w.travel, "give-mount", "mount", {}, {}, 200);
    std::set<std::string> distinct;
    for (const content::MountRow& m : w.world->mounts()->data().mounts) distinct.insert(m.item_name);
    CHECK(mounts.total == distinct.size() && option_with_value(mounts, "Mount_Grounded_Horse_0") != nullptr, "give-mount's mounts: every mount item once, by its item name");
    const GmOptions horse = options_of(*w.travel, "give-mount", "mount", {}, "horse");
    CHECK(!horse.options.empty() && horse.total < mounts.total && labels_hold(horse, "horse"), "... and a filter narrows them");

    const qs::QuestBook* book = w.world->quests();
    const GmOptions quests = options_of(*w.travel, "start-quest", "quest");
    CHECK(book && quests.total == book->data().by_name.size() && quests.options.size() == 50 && quests.more, "start-quest's quests: every quest by name, a page of 50 at a time");
    bool resolves = true;
    for (const GmFieldOption& o : quests.options) resolves = resolves && book->find(o.value) != nullptr;
    CHECK(resolves, "every quest value is a name the run call finds");
    const std::string some = book && !book->data().by_name.empty() ? book->data().by_name.begin()->first : std::string();
    const GmOptions named = options_of(*w.travel, "start-quest", "quest", {}, some);
    CHECK(!some.empty() && option_with_value(named, some) != nullptr, "a filter finds a quest by its name");

    const int64_t learnable = sql_number("SELECT COUNT(*) FROM craft_recipe WHERE learnable = 1 AND blacklisted = 0");
    const GmOptions recipes = options_of(*w.travel, "teach-recipe", "recipe");
    CHECK(learnable > 100 && static_cast<int64_t>(recipes.total) == learnable && recipes.options.size() == 50 && recipes.more, "teach-recipe's recipes: every recipe a player must learn, a page at a time");
    bool recipes_known = true;
    for (const GmFieldOption& o : recipes.options) recipes_known = recipes_known && w.world->craft_recipes()->recipe(std::strtoll(o.value.c_str(), nullptr, 10)) != nullptr;
    CHECK(recipes_known, "every recipe value is the Guid of a recipe of the data");
    const GmOptions forget = options_of(*w.travel, "forget-recipe", "recipe");
    CHECK(forget.options.empty() && forget.note == "Choose the player first.", "forget-recipe's recipes follow the character: none chosen, no list, and the note says so");
    const GmOptions absent = options_of(*w.travel, "forget-recipe", "recipe", {{"character", "nobody"}});
    CHECK(absent.options.empty() && absent.note.find("not in the world") != std::string::npos, "a character that is not in the world: the note says so");

    const int64_t stations = sql_number("SELECT COUNT(*) FROM craft_station_def");
    const GmOptions defs = options_of(*w.travel, "finish-jobs", "station");
    CHECK(stations > 0 && static_cast<int64_t>(defs.options.size()) == stations, "finish-jobs' stations: every station definition, by the name the command takes");

    // the creatures: only creatures, by name, with their place; a mount actor is no creature
    const auto add = [&](uint64_t id, world::ActorKind kind, const char* name, double x, double y) {
        world::WorldActor a;
        a.id = id;
        a.kind = kind;
        a.actor.guid.ObjectId = id;
        a.x = x;
        a.y = y;
        a.npc.name = name;
        a.npc.collision_cylinder = id + 1;   // (an actor's id is even; its cylinder's is odd, so that the two never meet)
        return w.world->registry().add(a);
    };
    CHECK(options_of(*w.travel, "npc-kill", "id").options.empty() && !options_of(*w.travel, "npc-kill", "id").note.empty(), "with no creature in the world the list is empty and says so");
    CHECK(add(9010, world::ActorKind::Npc, "Wolf", 150000, 250000) && add(9020, world::ActorKind::Npc, "Alpha Wolf", 100000, 50000) && add(9030, world::ActorKind::Npc, "Bear", 0, 0) &&
              add(9040, world::ActorKind::Mount, "Horse", 5, 5),
          "(three creatures and a mount join the registry)");
    const GmOptions all = options_of(*w.travel, "npc-heal", "id");
    CHECK(all.total == 3 && all.options.size() == 3 && value_at(all, 0) == "9020" && value_at(all, 1) == "9030" && value_at(all, 2) == "9010",
          "the creatures are listed by name (Alpha Wolf, Bear, Wolf), and the mount is not one");
    CHECK(label_at(all, 2).find("Wolf #9010") != std::string::npos && label_at(all, 2).find("(1500, 2500 m)") != std::string::npos, "each label has the name, the actor id and the place in metres");
    const GmOptions wolves = options_of(*w.travel, "npc-kill", "id", {}, "wolf");
    CHECK(wolves.total == 2 && wolves.options.size() == 2, "a filter keeps the creatures whose name holds the text");
    const GmOptions one = options_of(*w.travel, "npc-move", "id", {}, {}, 1, 1);
    CHECK(one.options.size() == 1 && value_at(one, 0) == "9030" && one.more && one.total == 3, "a page of 1 from the second: Bear, and there is more");
    const GmOptions by_id = options_of(*w.travel, "npc-reset", "id", {}, "9020");
    CHECK(by_id.total == 1 && value_at(by_id, 0) == "9020", "a creature is also found by its actor id");
    const GmOptions same_for_all = options_of(*w.travel, "npc-despawn", "id");
    CHECK(same_for_all.total == 3, "all six creature commands read the same list");
}
AOC_REGISTER_TEST(test_gm_options_world_lists);

// ---- the lists that follow a character: the quests it has taken and their objectives ---------------------------------------------------------------------------------

static void test_gm_options_taken_quests() {
    std::printf("the quests a character has taken, their active objectives, and the objective that must belong to the quest\n");
    std::string why;
    QuestPick pick;
    const bool have = pick.load(&why);
    CHECK(have, "the real content.db has a quest with an awake first objective");
    if (!have) return;
    // an awake objective of the second quest that the first does not have (the data shares 37 objective Guids between quests: the two quests' first objectives are one record)
    int64_t second_only = 0;
    {
        const content::QuestRow& a = pick.quest_data->quests.at(pick.first.quest);
        const content::QuestRow& b = pick.quest_data->quests.at(pick.second.quest);
        for (const content::QuestObjectiveRow& o : b.objectives) {
            const bool shared = std::any_of(a.objectives.begin(), a.objectives.end(), [&](const content::QuestObjectiveRow& x) { return x.guid == o.guid; });
            if (second_only == 0 && o.awake && !shared) second_only = o.guid;
        }
    }
    CHECK(second_only != 0, "(the second quest has an awake objective of its own)");
    if (second_only == 0) return;
    QuestStub api;
    qs::QuestWorker::Config wc;
    wc.poll_interval = milliseconds(20);
    auto worker = std::make_unique<qs::QuestWorker>(api, wc);
    svc::Character record = cyb_on_the_ground();
    record.quest_log = svc::SavedQuestLog{};
    Harness h(record, [&](PlayerWorld& w) {
        on_the_ground(w);
        w.set_dialogue(std::move(*pick.dialogue));
        w.set_quests(qs::QuestBook(std::move(*pick.quest_data)));
        w.set_quest_worker(worker.get());
    });
    h.play();
    GmTravel travel = make_travel(h);
    const auto run = [&](const char* command, std::map<std::string, std::string> fields) { return travel.run_command(command, fields, "the test"); };
    const std::map<std::string, std::string> cyb = {{"character", "cyb"}};
    const GmOptions none = options_of(travel, "complete-quest", "quest", cyb);
    CHECK(none.options.empty() && none.note.find("has taken no quest") != std::string::npos, "before a quest is taken: no quest to complete, and the note says so");
    const GmOptions nobody = options_of(travel, "complete-quest", "quest");
    CHECK(nobody.options.empty() && nobody.note == "Choose the player first.", "no character chosen: no list, and the note says to choose the player");
    const GmOptions gone = options_of(travel, "complete-quest", "quest", {{"character", "nobody"}});
    CHECK(gone.options.empty() && gone.note.find("not in the world") != std::string::npos, "a character that is not in the world: the note says so");
    CHECK(run("start-quest", {{"character", "cyb"}, {"quest", pick.first.name}, {"bypass", "yes"}}).status == GmStatus::Ok &&
              run("start-quest", {{"character", "cyb"}, {"quest", pick.second.name}, {"bypass", "yes"}}).status == GmStatus::Ok,
          "(two quests are taken)");
    h.settle();

    const GmOptions taken = options_of(travel, "complete-quest", "quest", cyb);
    CHECK(taken.options.size() == 2 && option_with_value(taken, pick.first.name) && option_with_value(taken, pick.second.name), "complete-quest's quests: the two the character has taken, by name");
    const GmOptions as_filter = options_of(travel, "complete-objective", "quest", cyb);
    CHECK(as_filter.options.size() == 2, "complete-objective's quest box lists the same two");
    const GmOptions first_only = options_of(travel, "complete-objective", "objective", {{"character", "cyb"}, {"quest", pick.first.name}});
    CHECK(option_with_value(first_only, std::to_string(pick.first.objective)) != nullptr, "the objectives follow the quest: the first quest's active objective is offered");
    bool all_first = !first_only.options.empty();
    for (const GmFieldOption& o : first_only.options) all_first = all_first && o.label.find(pick.second.name + ": ") == std::string::npos;
    CHECK(all_first && option_with_value(first_only, std::to_string(second_only)) == nullptr, "... and only that quest's: the second quest's own objective is not in its list");
    const GmOptions second_list = options_of(travel, "complete-objective", "objective", {{"character", "cyb"}, {"quest", pick.second.name}});
    CHECK(option_with_value(second_list, std::to_string(second_only)) != nullptr, "... the second quest's list has its own objective");
    const GmOptions both = options_of(travel, "complete-objective", "objective", cyb);
    CHECK(option_with_value(both, std::to_string(pick.first.objective)) && option_with_value(both, std::to_string(second_only)), "no quest chosen: the active objectives of every quest it has taken");
    const GmFieldOption* labelled = option_with_value(both, std::to_string(second_only));
    CHECK(labelled && labelled->label.rfind(pick.second.name + ": ", 0) == 0, "... each labelled with its quest's name");

    // an objective that belongs to another quest is refused when a quest is named, and runs when it belongs
    const GmRunResult wrong = run("complete-objective", {{"character", "cyb"}, {"quest", pick.first.name}, {"objective", std::to_string(second_only)}});
    CHECK(wrong.status == GmStatus::InvalidArgument && wrong.message.find("is not an objective of " + pick.first.name) != std::string::npos,
          "complete-objective with a quest: an objective of another quest is refused, and the answer says so");
    CHECK(run("complete-objective", {{"character", "cyb"}, {"quest", "No Such Quest"}, {"objective", std::to_string(pick.first.objective)}}).status == GmStatus::NotFound,
          "a quest that is none is NotFound");
    const GmRunResult right = run("complete-objective", {{"character", "cyb"}, {"quest", pick.first.name}, {"objective", std::to_string(pick.first.objective)}});
    CHECK(right.status == GmStatus::Ok, "complete-objective with its own quest runs (an option's value is a value the run call takes)");
    h.settle();
    // the first objective of both quests is one record: only the quest that was named has it completed
    const qs::QuestInstance* other = h.s->quest_log().find(pick.second.quest);
    bool other_done = false;
    if (other) {
        for (const qs::ObjectiveState& o : other->objectives) other_done = other_done || (o.record == pick.first.objective && o.completed);
    }
    CHECK(other != nullptr && !other_done, "the objective is completed in the quest that was named, not in another quest that has the same objective record");
    const GmOptions after = options_of(travel, "complete-objective", "objective", {{"character", "cyb"}, {"quest", pick.first.name}});
    CHECK(option_with_value(after, std::to_string(pick.first.objective)) == nullptr, "a completed objective is no longer offered");
}
AOC_REGISTER_TEST(test_gm_options_taken_quests);

// ---- over the admin API -------------------------------------------------------------------------------------------------------------------------------------------

static void test_gm_options_over_the_admin_api() {
    std::printf("the admin API: ListFieldOptions behind the token, applied on the network thread, and the list that says which fields have options\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    Admin a(h);
    const std::vector<adminv1::AdminCommand> commands = admin_command_list(a);
    const adminv1::AdminCommand* station = admin_command_named(commands, "place-station");
    const adminv1::CommandField* plot = station ? admin_field_named(*station, "plot") : nullptr;
    CHECK(plot && plot->has_options() && !plot->searchable() && plot->depends_on_size() == 1 && plot->depends_on(0) == "node", "ListCommands says place-station's plot has options that follow the node");
    const adminv1::AdminCommand* give = admin_command_named(commands, "give-item");
    const adminv1::CommandField* item = give ? admin_field_named(*give, "item") : nullptr;
    const adminv1::CommandField* count = give ? admin_field_named(*give, "count") : nullptr;
    CHECK(item && item->has_options() && item->searchable() && item->page_size() == 50 && count && !count->has_options(), "... that give-item's item is searchable with a page of 50, and its count is a free number");
    const auto call = [&](const std::string& command, const std::string& field, const std::map<std::string, std::string>& chosen, bool with_token, adminv1::ListFieldOptionsResponse* rs) {
        grpc::ClientContext ctx;
        if (with_token) Admin::authorize(ctx, kToken);
        adminv1::ListFieldOptionsRequest rq;
        rq.set_command(command);
        rq.set_field(field);
        for (const auto& [k, v] : chosen) (*rq.mutable_chosen())[k] = v;
        return a.via_net_thread([&] { return a.stub->ListFieldOptions(&ctx, rq, rs); });
    };
    adminv1::ListFieldOptionsResponse rs;
    CHECK(call("teleport", "place", {}, false, &rs).error_code() == grpc::StatusCode::UNAUTHENTICATED, "a call with no token is refused (Unauthenticated)");
    CHECK(call("teleport", "place", {}, true, &rs).ok() && rs.total() > 20, "ListFieldOptions answers teleport's places behind the token");
    bool login = false, joeva = false;
    for (const adminv1::FieldOption& o : rs.options()) {
        login = login || o.value() == "login";
        joeva = joeva || (o.value() == "Verra_RVR_Joeva" && o.label() == "Joeva");
    }
    CHECK(login && joeva && !rs.more(), "... with the login spot and Joeva, each a value and a label");
    CHECK(!a.directory_off_thread, "(the call touched no session off the network thread)");
    const adminv1::FieldOption* first_node = nullptr;
    for (const adminv1::FieldOption& o : rs.options()) {
        if (o.value() == "Verra_RVR_Joeva") first_node = &o;
    }
    const AdminRun moved = admin_run(a, "teleport", {{"character", "cyb"}, {"place", first_node ? first_node->value() : std::string("none")}});
    CHECK(moved.status.ok(), "a place value from the list is one the run call takes");
    adminv1::ListFieldOptionsResponse bad;
    CHECK(call("set-health", "value", {}, true, &bad).error_code() == grpc::StatusCode::INVALID_ARGUMENT, "a field with no options is InvalidArgument over the API");
    adminv1::ListFieldOptionsResponse plots;
    CHECK(call("place-station", "plot", {}, true, &plots).ok() && plots.options_size() == 0 && !plots.note().empty(), "a dependent list with nothing chosen: no option and the note (not an error)");
}
AOC_REGISTER_TEST(test_gm_options_over_the_admin_api);

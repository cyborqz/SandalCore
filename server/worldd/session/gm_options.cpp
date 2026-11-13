// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/gm_options.cpp  (the values a GM command's field can take; see gm_travel.h, GmTravel::field_options)
//
//  A form must not ask an operator for a name he has to know by heart.  Every field of the command list (session/admin_commands.h) whose value comes from a known set says so
//  (`has_options`) and this file reads that set from the world's own data: the places and the city nodes, the free plots of a node and the stations a plot can take, the station
//  definitions, the items (searched by text, a page at a time: the data holds thousands), the rarities one item can show, the recipes a character can be taught or was taught, the
//  weapon lines, the mounts, the quests, the quests a character has taken and their active objectives, and the creatures in the world.  Nothing is written here: the options are
//  read on the network thread like every other admin call, and each option's value is exactly what RunCommand takes for that field.
// ============================================================================
#include "session/gm_travel.h"

#include "content/content_db.h"
#include "items/rarity.h"
#include "quest/quest_book.h"
#include "session/admin_commands.h"
#include "session/weapon_lines.h"
#include "spawns/mount_spawn.h"
#include "spawns/node_population.h"
#include "world/city_nodes.h"
#include "world/places.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>

namespace aoc { namespace worldd {

namespace {

std::string lower_of(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool all_digits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; }); }

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string number_text(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

/// One list read whole, before the filter and the page: the options and the words that say why it is empty.
struct Whole {
    std::vector<GmFieldOption> options;
    std::string                note;
};

void sort_by_label(std::vector<GmFieldOption>& options) {
    std::sort(options.begin(), options.end(), [](const GmFieldOption& a, const GmFieldOption& b) {
        const std::string la = lower_of(a.label), lb = lower_of(b.label);
        return la != lb ? la < lb : a.value < b.value;
    });
}

/// The words of a rarity's quality band on an item's quality curve: "Rare (quality 16 to 30)".
std::string rarity_label(int rarity, const items::ItemRarityFacts& facts) {
    const items::QualityBand band = facts.has_curve ? facts.bands[static_cast<size_t>(rarity)]
                                                    : items::QualityBand{static_cast<double>(items::first_quality_of(rarity)), static_cast<double>(items::last_quality_of(rarity))};
    return std::string(items::rarity_name(rarity)) + " (quality " + number_text(std::ceil(band.x)) + " to " + number_text(std::floor(band.y)) + ")";
}

/// The item a GM names: its Guid in digits or its record name (one item only).  nullopt, with `note` in plain words, for none.
std::optional<content::ItemRecord> item_named(const content::ContentDb& db, const std::string& ref, std::string& note) {
    std::string error;
    int64_t guid = 0;
    if (all_digits(ref) && ref.size() <= 19) {
        errno = 0;
        guid = static_cast<int64_t>(std::strtoll(ref.c_str(), nullptr, 10));
    } else {
        const std::vector<int64_t> found = db.items_named(ref, error);
        if (!error.empty()) {
            note = error;
            return std::nullopt;
        }
        if (found.size() != 1) {
            note = found.empty() ? "No item is named \"" + ref + "\"." : "\"" + ref + "\" names several items: pick one from the list.";
            return std::nullopt;
        }
        guid = found.front();
    }
    std::optional<content::ItemRecord> rec = db.item(guid, error);
    if (!rec) note = error.empty() ? "The data holds no item " + ref + "." : error;
    return rec;
}

/// The creatures a game master can act on, before the labels: the registry's creatures with the facts the filter and the order read.
struct Creature {
    uint64_t    id = 0;
    std::string name;
    double      x = 0, y = 0;
};

}  // namespace

GmOptions GmTravel::field_options(const std::string& command, const std::string& field_name, const std::map<std::string, std::string>& chosen, const std::string& filter, size_t limit,
                                  size_t offset) {
    GmOptions out;
    const auto refuse = [&out](std::string message) {
        out.status = GmStatus::InvalidArgument;
        out.message = std::move(message);
        return out;
    };
    const admin::AdminCommand* def = admin::find_command(command);
    if (!def) return refuse("no command is named \"" + log_text(command) + "\" (ListCommands lists them)");
    const admin::CommandField* field = nullptr;
    for (const admin::CommandField& f : def->fields) {
        if (f.name == field_name) field = &f;
    }
    if (!field) return refuse("the command " + def->name + " has no field \"" + log_text(field_name) + "\"");
    if (!field->has_options) return refuse("the field \"" + field->name + "\" of " + def->name + " takes a free value: it has no list of options");

    // what is chosen so far, as a lookup that is never longer than a field may be
    const auto pick = [&chosen](const char* name) -> std::string {
        const auto it = chosen.find(name);
        return it == chosen.end() || it->second.size() > 256 ? std::string() : trimmed(it->second);
    };
    const std::string key = def->name + "." + field->name;
    const std::string wanted = lower_of(filter);
    const auto matches = [&wanted](const std::string& label, const std::string& value) {
        return wanted.empty() || lower_of(label).find(wanted) != std::string::npos || lower_of(value).find(wanted) != std::string::npos;
    };
    size_t page = limit == 0 ? (field->page_size != 0 ? field->page_size : admin::kOptionsPageSize) : limit;
    page = std::min<size_t>(std::max<size_t>(page, 1), admin::kOptionsMaxPage);

    // ---- the lists that are read by text and a page from the data itself ----------------------------------------------------------------------------------------------
    if (key == "give-item.item") {
        const content::ContentDb* db = world_.content();
        if (!db) {
            out.note = "This world has no content.db to list the items from.";
            return out;
        }
        std::string error;
        size_t total = 0;
        const std::vector<content::ContentDb::ItemListRow> rows = db->list_items(filter, page, offset, total, error);
        if (!error.empty()) {
            out.status = GmStatus::FailedPrecondition;
            out.message = error;
            return out;
        }
        for (const content::ContentDb::ItemListRow& r : rows) {
            const std::string shown = r.display_name.empty() ? r.name : r.display_name;
            out.options.push_back({std::to_string(r.guid), shown == r.name ? shown : shown + " (" + r.name + ")"});
        }
        out.total = total;
        out.more = offset + rows.size() < total;
        if (total == 0) out.note = filter.empty() ? "The data holds no items." : "No item matches \"" + log_text(filter) + "\".";
        return out;
    }

    // ---- the lists read whole from the world, then filtered and paged here -------------------------------------------------------------------------------------------
    Whole whole;
    const auto player = [&]() -> PlayerHandle { return find_player(pick("character")); };

    if (key == "teleport.place") {
        const world::Places places = world_.places();   // (a copy: places() builds the list anew, and a range-for over a temporary's member would dangle)
        for (const world::Place& p : places.all()) {
            whole.options.push_back({p.name, p.display_name.empty() ? p.name : p.display_name});
        }
        sort_by_label(whole.options);
    } else if (key == "place-station.node" || key == "node-xp.node" || key == "remove-station.node") {
        const world::CityNodes* nodes = world_.city_nodes();
        if (!nodes) {
            whole.note = "This world has no city nodes.";
        } else {
            for (const world::CityNodeInfo& n : nodes->nodes()) {
                if (!world::has_travel_place(n)) continue;   // the test nodes and the dungeons have no place on the map
                const int level = nodes->level_of(n.guid);
                if (key == "place-station.node" && level < 3) continue;   // only a Village or a Town has service plots
                if (key == "remove-station.node") {
                    std::string why;
                    if (world_.station_plots(n.name, true, why).empty()) continue;
                }
                const std::string shown = n.display_name.empty() ? n.name : n.display_name;
                whole.options.push_back({n.name, shown + " (" + spawns::node_level_name(level) + ")"});
            }
            sort_by_label(whole.options);
            if (whole.options.empty()) {
                whole.note = key == "place-station.node" ? "No city node is a Village or a Town now, so no station can be placed." : key == "remove-station.node" ? "No station was placed." : "This world lists no city node.";
            }
        }
    } else if (key == "place-station.plot" || key == "remove-station.plot") {
        const bool placed = key == "remove-station.plot";
        const std::vector<PlayerWorld::StationPlot> plots = world_.station_plots(pick("node"), placed, whole.note);
        for (const PlayerWorld::StationPlot& p : plots) whole.options.push_back({p.plot, placed && !p.station.empty() ? p.plot + " (" + p.station + ")" : p.plot});
        sort_by_label(whole.options);
    } else if (key == "place-station.station") {
        for (const std::string& name : world_.plot_station_names(pick("node"), pick("plot"), whole.note)) whole.options.push_back({name, name});
        sort_by_label(whole.options);
    } else if (key == "finish-jobs.station") {
        const content::CraftStationData* data = world_.station_data();
        if (!data) {
            whole.note = "This world has no station definitions.";
        } else {
            for (const auto& [guid, d] : data->defs) {
                whole.options.push_back({d.name, d.display_name.empty() || d.display_name == d.name ? d.name : d.display_name + " (" + d.name + ")"});
            }
            sort_by_label(whole.options);
        }
    } else if (key == "give-item.rarity") {
        items::ItemRarityFacts facts = items::default_rarity_facts();
        int lo = 0, hi = items::kRarityCount - 1;
        const std::string ref = pick("item");
        const content::ContentDb* db = world_.content();
        bool known = true;
        if (!ref.empty() && db) {
            const std::optional<content::ItemRecord> rec = item_named(*db, ref, whole.note);
            if (!rec) {
                known = false;
            } else if (!rec->has_quality) {
                known = false;
                whole.note = (rec->display_name.empty() ? rec->name : rec->display_name) + " has no quality in the game's data: its rarity is fixed, so give it without one.";
            } else {
                facts = *rec;
                lo = facts.rarity_min;
                hi = facts.has_curve ? std::max(facts.rarity_min, facts.rarity_max) : facts.rarity_min;
                const std::string shown = rec->display_name.empty() ? rec->name : rec->display_name;
                whole.note = lo == hi ? shown + " is always " + std::string(items::rarity_name(lo)) + "." : "";
            }
        }
        for (int r = lo; known && r <= hi; ++r) whole.options.push_back({std::string(items::rarity_name(r)), rarity_label(r, facts)});
    } else if (key == "teach-recipe.recipe" || key == "forget-recipe.recipe") {
        const content::CraftRecipeData* data = world_.craft_recipes();
        if (!data) {
            whole.note = "worldd has no recipes (an older content.db).";
        } else if (key == "teach-recipe.recipe") {
            std::set<int64_t> known;
            if (const PlayerHandle h = player()) {
                for (const int64_t g : h.player->taught_recipes()) known.insert(g);
            }
            for (const auto& [guid, r] : data->recipes) {
                if (!r.learnable || r.blacklisted || known.count(guid)) continue;
                whole.options.push_back({std::to_string(guid), r.name});
            }
            sort_by_label(whole.options);
        } else if (const PlayerHandle h = player()) {
            for (const int64_t g : h.player->taught_recipes()) {
                const content::CraftRecipe* r = data->recipe(g);
                whole.options.push_back({std::to_string(g), r ? r->name : std::to_string(g)});
            }
            sort_by_label(whole.options);
            if (whole.options.empty()) whole.note = h.player->name() + " was taught no recipe.";
        } else {
            whole.note = pick("character").empty() ? "Choose the player first." : pick("character") + " is not in the world.";
        }
    } else if (key == "weapon-xp.line") {
        const weaponxp::Lines* lines = world_.weapon_lines();
        if (!lines) {
            whole.note = "This world has no weapon lines (its content.db lacks their leveling types).";
        } else {
            for (const weaponxp::Line& l : lines->all()) whole.options.push_back({l.name, l.name});
            sort_by_label(whole.options);
        }
    } else if (key == "give-mount.mount") {
        const spawns::MountBook* book = world_.mounts();
        if (!book) {
            whole.note = "This world has no mounts (its content.db lacks the mount tables).";
        } else {
            std::set<std::string> seen;
            for (const content::MountRow& m : book->data().mounts) {
                if (m.item_name.empty() || !seen.insert(lower_of(m.item_name)).second) continue;
                std::string label = m.display_name.empty() ? m.item_name : m.display_name + " (" + m.item_name + ")";
                if (!m.kind.empty()) label += ", " + m.kind;
                if (m.level > 0) label += ", level " + std::to_string(m.level);
                whole.options.push_back({m.item_name, label});
            }
            sort_by_label(whole.options);
        }
    } else if (key == "start-quest.quest") {
        const quest::QuestBook* book = world_.quests();
        if (!book) {
            whole.note = "This world has no quests (its content.db is older than the quest tables).";
        } else {
            for (const auto& [name, guid] : book->data().by_name) {
                const content::QuestRow* q = book->quest(guid);
                whole.options.push_back({name, q && q->level > 0 ? name + " (level " + std::to_string(q->level) + ")" : name});
            }
            sort_by_label(whole.options);
        }
    } else if (key == "complete-quest.quest" || key == "complete-objective.quest" || key == "complete-objective.objective") {
        const quest::QuestBook* book = world_.quests();
        const PlayerHandle h = player();
        if (!book) {
            whole.note = "This world has no quests (its content.db is older than the quest tables).";
        } else if (!h) {
            whole.note = pick("character").empty() ? "Choose the player first." : pick("character") + " is not in the world.";
        } else if (key != "complete-objective.objective") {
            for (const quest::QuestInstance& q : h.player->quest_log().instances()) {
                const content::QuestRow* row = book->quest(q.record);
                if (!row) continue;
                whole.options.push_back({row->name, row->level > 0 ? row->name + " (level " + std::to_string(row->level) + ")" : row->name});
            }
            sort_by_label(whole.options);
            if (whole.options.empty()) whole.note = h.player->name() + " has taken no quest.";
        } else {
            // the active objectives of the quests the character has taken: of the quest chosen, or of all of them
            const std::string ref = pick("quest");
            const content::QuestRow* only = ref.empty() ? nullptr : book->find(ref);
            if (!ref.empty() && !only) {
                whole.note = "No quest is named \"" + log_text(ref) + "\".";
            } else {
                for (const quest::QuestInstance& q : h.player->quest_log().instances()) {
                    const content::QuestRow* row = book->quest(q.record);
                    if (!row || (only && row->guid != only->guid)) continue;
                    for (const quest::ObjectiveState& o : q.objectives) {
                        if (!o.active || o.completed) continue;
                        const auto def_it = std::find_if(row->objectives.begin(), row->objectives.end(), [&o](const content::QuestObjectiveRow& d) { return d.guid == o.record; });
                        std::string label = def_it == row->objectives.end() ? std::to_string(o.record) : def_it->name + (def_it->type.empty() ? std::string() : " (" + def_it->type + ")");
                        if (def_it != row->objectives.end() && def_it->count > 1) label += " " + std::to_string(o.count) + " of " + std::to_string(def_it->count);
                        whole.options.push_back({std::to_string(o.record), only ? label : row->name + ": " + label});
                    }
                }
                sort_by_label(whole.options);
                if (whole.options.empty()) {
                    whole.note = only ? h.player->name() + " has no active objective in " + only->name + "." : h.player->name() + " has no active objective in any quest it has taken.";
                }
            }
        }
    } else if (def->name.rfind("npc-", 0) == 0 && field->name == "id") {
        std::vector<Creature> found;
        world_.registry().for_each([&](const world::WorldActor& a) {
            if (a.kind != world::ActorKind::Npc) return;
            const std::string name = gm_creature_name(a);
            if (!matches(name + " " + std::to_string(a.id), std::to_string(a.id))) return;
            found.push_back({a.id, name, a.x, a.y});
        });
        std::sort(found.begin(), found.end(), [](const Creature& a, const Creature& b) {
            const std::string la = lower_of(a.name), lb = lower_of(b.name);
            return la != lb ? la < lb : a.id < b.id;
        });
        out.total = found.size();
        const size_t from = std::min(offset, found.size()), to = std::min(found.size(), from + page);
        for (size_t i = from; i < to; ++i) {
            const Creature& c = found[i];
            char where[64];
            std::snprintf(where, sizeof where, "(%.0f, %.0f m)", c.x / 100.0, c.y / 100.0);
            out.options.push_back({std::to_string(c.id), c.name + " #" + std::to_string(c.id) + " " + where + (gm_creature_dead(world_, c.id) ? ", dead" : "")});
        }
        out.more = to < found.size();
        if (found.empty()) out.note = filter.empty() ? "No creature is in the world." : "No creature matches \"" + log_text(filter) + "\".";
        return out;
    } else {
        return refuse("the field \"" + field->name + "\" of " + def->name + " lists options, but this worldd has no source for them");
    }

    // ---- the filter and the page of a list read whole ----------------------------------------------------------------------------------------------------------------
    out.note = whole.note;
    if (!field->searchable) {
        out.total = whole.options.size();
        for (size_t i = 0; i < whole.options.size() && i < admin::kOptionsListCap; ++i) out.options.push_back(std::move(whole.options[i]));
        if (whole.options.size() > admin::kOptionsListCap) out.note = "The list is cut at " + std::to_string(admin::kOptionsListCap) + " options.";
        return out;
    }
    std::vector<GmFieldOption> kept;
    for (GmFieldOption& o : whole.options) {
        if (matches(o.label, o.value)) kept.push_back(std::move(o));
    }
    out.total = kept.size();
    const size_t from = std::min(offset, kept.size()), to = std::min(kept.size(), from + page);
    for (size_t i = from; i < to; ++i) out.options.push_back(std::move(kept[i]));
    out.more = to < kept.size();
    if (kept.empty() && !filter.empty() && out.note.empty()) out.note = "No option matches \"" + log_text(filter) + "\".";
    return out;
}

}}  // namespace aoc::worldd

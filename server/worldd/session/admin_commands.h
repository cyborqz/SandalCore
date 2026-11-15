// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/admin_commands.h  (the GM command list and its field rules)
//
//  One list of every command the WorldAdminService offers: its name, one sentence in plain words, its group, whether it names a character, and its fields.  The admin app draws a
//  form for a command from this list (proto/internal/aoc/worldadmin/v1/worldadmin.proto, ListCommands), so a command added here shows up there without a change to the app.
//
//  A field says how its value is written (a whole number, a number, text, a character, an item, a place, or one of a choice's values), whether it is required, and its range or its
//  values.  RunCommand carries every value as text and check_command_fields() holds it to those rules before anything runs: a name that is not in the list, a field the command
//  does not have, a missing required field, a value that is not its type or is outside its range, and a choice that is not one of its values are all refused, whoever calls.
// ============================================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace aoc::worldd::admin {

/// How a field's text is read (the proto's CommandFieldType has the same values).
enum class FieldType : uint8_t {
    WholeNumber,
    Number,
    Text,
    Character,
    Item,
    Place,
    Choice,
};

struct CommandField {
    std::string              name;        // the key RunCommand.fields carries the value under
    std::string              label;       // what the form calls it
    FieldType                type = FieldType::Text;
    bool                     required = true;
    std::string              hint;        // where the value comes from, or an example
    std::vector<std::string> choices;     // FieldType::Choice: the values allowed
    bool                     has_minimum = false;
    double                   minimum = 0;
    bool                     has_maximum = false;
    double                   maximum = 0;
    // Values from the world's data (GmTravel::field_options, WorldAdmin ListFieldOptions): a form draws a drop-down instead of a box.  A field that has options still holds the type's rules (check_command_fields).
    bool                     has_options = false;
    bool                     searchable = false;   // a long list: read by filter text and page
    uint32_t                 page_size = 0;        // searchable: the options of one page
    std::vector<std::string> depends_on;           // the fields of the command whose chosen values change the list
};

/// The options of one page of a searchable field, and the most a call may ask for.
inline constexpr uint32_t kOptionsPageSize = 50;
inline constexpr uint32_t kOptionsMaxPage = 200;
/// The most options a field that is not searchable answers (its list is short; the cap only guards the answer's size).
inline constexpr size_t kOptionsListCap = 1000;

struct AdminCommand {
    std::string                name;              // "teleport", "heal", "npc-kill"
    std::string                description;       // one sentence in plain words
    std::string                group;             // "movement", "character", "combat", "quests" ...
    bool                       acts_on_character = false;
    std::vector<CommandField>  fields;
};

/// A field's value, read: its text as it came (a choice is the list's own spelling) and its number when its type is one.
struct FieldValue {
    std::string text;
    int64_t     whole = 0;
    double      number = 0;
};

using FieldValues = std::map<std::string, FieldValue>;

inline CommandField character_field(const char* hint = "the character's name (ListPlayers lists the players in the world)") {
    CommandField f;
    f.name = "character";
    f.label = "Character";
    f.type = FieldType::Character;
    f.required = true;
    f.hint = hint;
    return f;
}

inline CommandField text_field(const char* name, const char* label, bool required, const char* hint) {
    CommandField f;
    f.name = name;
    f.label = label;
    f.type = FieldType::Text;
    f.required = required;
    f.hint = hint;
    return f;
}

inline CommandField item_field(const char* name, const char* label, bool required, const char* hint) {
    CommandField f = text_field(name, label, required, hint);
    f.type = FieldType::Item;
    return f;
}

inline CommandField place_field(const char* name, const char* label, bool required, const char* hint) {
    CommandField f = text_field(name, label, required, hint);
    f.type = FieldType::Place;
    return f;
}

inline CommandField whole_field(const char* name, const char* label, bool required, const char* hint, bool has_min, double min, bool has_max, double max) {
    CommandField f;
    f.name = name;
    f.label = label;
    f.type = FieldType::WholeNumber;
    f.required = required;
    f.hint = hint;
    f.has_minimum = has_min;
    f.minimum = min;
    f.has_maximum = has_max;
    f.maximum = max;
    return f;
}

inline CommandField number_field(const char* name, const char* label, bool required, const char* hint, bool has_min, double min, bool has_max, double max) {
    CommandField f = whole_field(name, label, required, hint, has_min, min, has_max, max);
    f.type = FieldType::Number;
    return f;
}

/// Says that a field's values come from the world's data: `depends_on` names the fields whose chosen values change the list, `searchable` marks a long list that is read by filter and page.
inline CommandField with_options(CommandField f, std::initializer_list<const char*> depends_on = {}, bool searchable = false) {
    f.has_options = true;
    f.searchable = searchable;
    f.page_size = searchable ? kOptionsPageSize : 0;
    for (const char* d : depends_on) f.depends_on.emplace_back(d);
    return f;
}

inline CommandField choice_field(const char* name, const char* label, bool required, const char* hint, std::initializer_list<const char*> choices) {
    CommandField f;
    f.name = name;
    f.label = label;
    f.type = FieldType::Choice;
    f.required = required;
    f.hint = hint;
    for (const char* c : choices) f.choices.emplace_back(c);
    return f;
}

/// Every command the admin API offers, in the order the page shows them: the travel and NPC tools first (they were there before), then the game-master test commands.
inline const std::vector<AdminCommand>& admin_commands() {
    static const std::vector<AdminCommand> list = {
        {"list-players", "Lists every player in the world, with its position, whether it is mounted and its run speed.", "world", false, {}},
        {"list-places", "Lists the places teleport takes by name (the city nodes and the login spot).", "movement", false, {}},
        {"teleport", "Moves a character to a place by name or to a point (cm); without z the character stands on the ground there.", "movement", true,
         {character_field("the character to move"),
          with_options(place_field("place", "Place", false, "a place ListPlaces lists, such as Joeva or login; empty when a point is given")),
          number_field("x", "X", false, "the point's x in cm", false, 0, false, 0),
          number_field("y", "Y", false, "the point's y in cm", false, 0, false, 0),
          number_field("z", "Z", false, "the point's z in cm; empty: the ground under the point", false, 0, false, 0)}},
        {"speed", "Sets a character's run speed, a multiplier of its normal speed: 0.5 to 10, and 1 puts it back.", "movement", true,
         {character_field(), number_field("multiplier", "Multiplier", true, "0.5 to 10; 1 resets", true, 0.5, true, 10)}},
        {"give-money", "Adds copper to a character's purse, up to the purse's cap.", "items", true,
         {character_field(), whole_field("copper", "Copper", true, "1 to 10,000,000 (100 copper = 1 silver, 100 silver = 1 gold)", true, 1, true, 10000000)}},
        {"give-item", "Gives a character an item by its record name or Guid, in stacks of its own stack size and into the bag its kind belongs to.", "items", true,
         {character_field(), with_options(item_field("item", "Item", true, "the item's record name or its Guid"), {}, true),
          whole_field("count", "Count", false, "1 to 100,000; empty means 1", true, 1, true, 100000),
          with_options(choice_field("rarity", "Rarity", false, "the colour the item is given in; empty: its own quality", {"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"}), {"item"}),
          whole_field("quality", "Quality", false, "an exact quality, 1 to 200; empty: its own", true, 1, true, 200)}},
        {"place-station", "Puts an artisan building's station on a free service plot of a Village or a Town; it stays until it is removed or worldd restarts.", "crafting", false,
         {with_options(text_field("node", "Node", true, "the city node's name, such as Joeva")), with_options(text_field("plot", "Plot", true, "a free service plot, such as Plot_17_Service"), {"node"}),
          with_options(text_field("station", "Station", true, "the station's asset or definition name"), {"node", "plot"})}},
        {"remove-station", "Takes a station placed by place-station off its plot.", "crafting", false,
         {with_options(text_field("node", "Node", true, "the city node's name")), with_options(text_field("plot", "Plot", true, "the plot the station stands on"), {"node"})}},
        {"finish-jobs", "Finishes a character's processing jobs that are not over, so they are ready to collect; with a station, only the jobs there.", "crafting", true,
         {character_field(), with_options(text_field("station", "Station", false, "a station's Blueprint or definition name; empty: every station"))}},
        {"teach-recipe", "Teaches a character a recipe the game makes a player learn, by the recipe's name or Guid.", "crafting", true,
         {character_field(), with_options(text_field("recipe", "Recipe", true, "the recipe's record name or its Guid"), {"character"}, true)}},
        {"forget-recipe", "Takes a taught recipe away from a character again.", "crafting", true,
         {character_field(), with_options(text_field("recipe", "Recipe", true, "the recipe's record name or its Guid"), {"character"}, true)}},
        {"list-taught-recipes", "Lists the recipes a character was taught.", "crafting", true, {character_field()}},
        {"reset-tutorials", "Puts a character's finished tutorials back, so its tutorial hints can start again.", "character", true, {character_field()}},
        {"weapon-xp", "Sets a character's weapon line experience by level (1 to 25) or by the XP total itself.", "character", true,
         {character_field(), with_options(text_field("line", "Weapon line", true, "the line's name, the same without Weapon_, or a word that ends it (wand)")),
          whole_field("level", "Level", false, "1 to 25; empty when xp is given", true, 1, true, 25),
          whole_field("xp", "XP total", false, "0 to 5,718,160; empty when level is given", true, 0, true, 5718160)}},
        {"npc-kill", "Kills a creature as a player's blow would: the same corpse, loot and respawn timer.", "creatures", false,
         {with_options(whole_field("id", "Creature id", true, "the creature's actor id (the world stream and Inspect give it)", true, 1, true, 9.2e18), {}, true)}},
        {"npc-respawn", "Makes a dead creature again at its spawn point now, or replaces a living one by a fresh one there.", "creatures", false,
         {with_options(whole_field("id", "Creature id", true, "the creature's actor id", true, 1, true, 9.2e18), {}, true)}},
        {"npc-reset", "Sends a living creature back to its spawn point at full health, with its threat cleared.", "creatures", false,
         {with_options(whole_field("id", "Creature id", true, "the creature's actor id", true, 1, true, 9.2e18), {}, true)}},
        {"npc-heal", "Puts a living creature's health to full.", "creatures", false,
         {with_options(whole_field("id", "Creature id", true, "the creature's actor id", true, 1, true, 9.2e18), {}, true)}},
        {"npc-despawn", "Takes a creature out of the world now; its spawn point makes it again on its timer.", "creatures", false,
         {with_options(whole_field("id", "Creature id", true, "the creature's actor id", true, 1, true, 9.2e18), {}, true)}},
        {"npc-move", "Stands a living creature at a point (cm); its leash is centred there and its spawn point stays.", "creatures", false,
         {with_options(whole_field("id", "Creature id", true, "the creature's actor id", true, 1, true, 9.2e18), {}, true),
          number_field("x", "X", true, "the point's x in cm", false, 0, false, 0), number_field("y", "Y", true, "the point's y in cm", false, 0, false, 0),
          number_field("z", "Z", false, "the point's z in cm; empty: the ground under it", false, 0, false, 0)}},

        // ---- the game-master test commands (the client's own console commands, reached from the page) ----
        {"heal", "Puts a character's Health and Mana back to their maximum.", "character", true, {character_field()}},
        {"set-health", "Sets a living character's Health; 0 kills it, a value above the maximum is held at the maximum.", "character", true,
         {character_field(), number_field("value", "Health", true, "0 or more; above the maximum is the maximum", true, 0, true, 10000000)}},
        {"set-mana", "Sets a living character's Mana; a value above the maximum is held at the maximum.", "character", true,
         {character_field(), number_field("value", "Mana", true, "0 or more; above the maximum is the maximum", true, 0, true, 10000000)}},
        {"revive", "Brings a dead character back at the nearest place it may return, alive and with full Health and Mana.", "character", true, {character_field()}},
        {"kill", "Kills a character the way a blow does: the same death, experience debt, dropped materials and respawn choices.", "combat", true, {character_field()}},
        {"give-experience", "Adds Adventuring experience to a character; the levels it crosses are applied and the client is told.", "character", true,
         {character_field(), whole_field("amount", "Experience", true, "1 or more", true, 1, true, 1000000000)}},
        {"start-quest", "Starts a quest for a character by its name or Guid, as the game's own debug command does.", "quests", true,
         {character_field(), with_options(text_field("quest", "Quest", true, "the quest's record name or its Guid"), {}, true),
          choice_field("bypass", "Bypass rules", false, "yes: take it even when its start rules do not hold; empty means no", {"yes", "no"})}},
        {"complete-quest", "Finishes a quest a character has taken, as if its objectives were done.", "quests", true,
         {character_field(), with_options(text_field("quest", "Quest", true, "the quest's record name or its Guid"), {"character"}, true)}},
        {"complete-objective", "Completes one objective of a quest a character has taken, by the objective's Guid.", "quests", true,
         {character_field(), with_options(text_field("quest", "Quest", false, "a quest the character has taken; it narrows the objectives; empty: the objectives of every quest it has taken"), {"character"}, true),
          with_options(whole_field("objective", "Objective", true, "the objective's record Guid", true, 1, true, 9.2e18), {"character", "quest"})}},
        {"reset-quests", "Forgets a character's finished quests and quest variables; its quest log is sent again.", "quests", true, {character_field()}},
        {"clear-cooldowns", "Ends every cooldown running on a character's abilities now.", "combat", true, {character_field()}},
        {"give-mount", "Gives a character the mount item of a mount by its item or Blueprint name; the character summons it as it summons any mount.", "mounts", true,
         {character_field(), with_options(text_field("mount", "Mount", true, "the mount item's name or its Blueprint's, such as Mount_Grounded_Horse_0"), {}, true)}},
        {"set-time", "Sets the world's time of day (the hour, 0 to 24) and sends the new clock to every player.", "world", false,
         {number_field("hour", "Hour", true, "0 to 24 (24 is midnight); 7.5 is half past seven", true, 0, true, 24)}},
        {"node-xp", "Asks the city node service to add experience to a node.", "nodes", false,
         {with_options(text_field("node", "Node", true, "the city node's name, such as Joeva")), whole_field("amount", "Experience", true, "1 or more", true, 1, true, 2147483647)}},
    };
    return list;
}

/// The command named `name` (case does not count), or nullptr.
inline const AdminCommand* find_command(const std::string& name) {
    for (const AdminCommand& c : admin_commands()) {
        if (c.name.size() != name.size()) continue;
        bool same = true;
        for (size_t i = 0; i < name.size() && same; ++i) same = std::tolower(static_cast<unsigned char>(c.name[i])) == std::tolower(static_cast<unsigned char>(name[i]));
        if (same) return &c;
    }
    return nullptr;
}

namespace detail {

inline std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

inline bool parse_whole(const std::string& text, int64_t* out) {
    const std::string s = trimmed(text);
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0') return false;
    *out = static_cast<int64_t>(v);
    return true;
}

inline bool parse_number(const std::string& text, double* out) {
    const std::string s = trimmed(text);
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (errno == ERANGE || end == nullptr || *end != '\0' || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

inline std::string number_text(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

inline std::string range_text(const CommandField& f) {
    if (f.has_minimum && f.has_maximum) return number_text(f.minimum) + " to " + number_text(f.maximum);
    if (f.has_minimum) return number_text(f.minimum) + " or more";
    if (f.has_maximum) return number_text(f.maximum) + " or less";
    return "in range";
}

}  // namespace detail

/// Holds `raw` to `command`'s own field rules: a field the command does not have, a missing required field, a value that is not its type, is outside its range, or is not one of its
/// choices is refused with a sentence in `error` and nothing is filled.  An empty value counts as no value (a form may send one).  True, with every field read into `out`, otherwise.
inline bool check_command_fields(const AdminCommand& command, const std::map<std::string, std::string>& raw, FieldValues* out, std::string* error) {
    FieldValues values;
    for (const auto& [name, text] : raw) {
        const CommandField* field = nullptr;
        for (const CommandField& f : command.fields) {
            if (f.name == name) {
                field = &f;
                break;
            }
        }
        if (!field) {
            if (error) *error = "the command " + command.name + " has no field \"" + name + "\"";
            return false;
        }
        if (text.empty()) continue;   // no value
        FieldValue v;
        v.text = text;
        switch (field->type) {
            case FieldType::WholeNumber:
                if (!detail::parse_whole(text, &v.whole)) {
                    if (error) *error = "\"" + field->label + "\" must be a whole number (it is \"" + text + "\")";
                    return false;
                }
                if ((field->has_minimum && static_cast<double>(v.whole) < field->minimum) || (field->has_maximum && static_cast<double>(v.whole) > field->maximum)) {
                    if (error) *error = "\"" + field->label + "\" must be " + detail::range_text(*field) + " (it is " + text + ")";
                    return false;
                }
                v.number = static_cast<double>(v.whole);
                break;
            case FieldType::Number:
                if (!detail::parse_number(text, &v.number)) {
                    if (error) *error = "\"" + field->label + "\" must be a number (it is \"" + text + "\")";
                    return false;
                }
                if ((field->has_minimum && v.number < field->minimum) || (field->has_maximum && v.number > field->maximum)) {
                    if (error) *error = "\"" + field->label + "\" must be " + detail::range_text(*field) + " (it is " + text + ")";
                    return false;
                }
                break;
            case FieldType::Choice: {
                const std::string word = detail::trimmed(text);
                std::string canonical;
                for (const std::string& c : field->choices) {
                    if (detail::iequals(c, word)) {
                        canonical = c;
                        break;
                    }
                }
                if (canonical.empty()) {
                    if (error) {
                        *error = "\"" + field->label + "\" must be one of:";
                        for (const std::string& c : field->choices) *error += " " + c;
                    }
                    return false;
                }
                v.text = canonical;
                break;
            }
            case FieldType::Text:
            case FieldType::Character:
            case FieldType::Item:
            case FieldType::Place:
                break;
        }
        values.emplace(name, std::move(v));
    }
    for (const CommandField& f : command.fields) {
        const auto it = values.find(f.name);
        if (it == values.end()) {
            if (f.required) {
                if (error) *error = "\"" + f.label + "\" is required" + (f.hint.empty() ? std::string() : " (" + f.hint + ")");
                return false;
            }
            continue;
        }
        if (f.type == FieldType::Text || f.type == FieldType::Character || f.type == FieldType::Item || f.type == FieldType::Place) {
            const size_t limit = f.type == FieldType::Character ? 128 : 256;
            if (it->second.text.size() > limit) {
                if (error) *error = "\"" + f.label + "\" is longer than " + std::to_string(limit) + " characters";
                return false;
            }
        }
    }
    if (out) *out = std::move(values);
    return true;
}

/// The value of `name` in `values`, or nullptr when the command was not given it.
inline const FieldValue* field_value(const FieldValues& values, const char* name) {
    const auto it = values.find(name);
    return it == values.end() ? nullptr : &it->second;
}

}  // namespace aoc::worldd::admin

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_display_names.inc: what the mob log lines call a creature (included in combat_world.cpp, inside its namespace)
//
//  The game shows a creature by the name its population data gives its instance or asset, else by the name its Blueprint carries (the table tools/npcabilities writes).  A person the data names
//  has that name on its spawn already.  The log lines carry the name with the Blueprint's leaf beside it ("Slate Wolf (Wolf)"); nothing here reaches a client.
// ============================================================================

size_t CombatWorld::load_display_names(const std::string& path) {
    display_names_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;   // (no table: the log lines keep the Blueprint's leaf)
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (header) { header = false; continue; }
        const size_t first = line.find('\t');
        if (first == std::string::npos) continue;
        const size_t second = line.find('\t', first + 1);
        if (second == std::string::npos || second == first + 1) continue;
        display_names_[line.substr(0, first)] = line.substr(first + 1, second - first - 1);
    }
    return display_names_.size();
}

const std::string& CombatWorld::display_name(const std::string& archetype) const {
    static const std::string none;
    const auto it = display_names_.find(archetype);
    return it == display_names_.end() ? none : it->second;
}

std::string CombatWorld::log_name(const std::string& archetype, const std::string& leaf) const {
    const std::string& shown = display_name(archetype);
    return shown.empty() || shown == leaf ? leaf : shown + " (" + leaf + ")";
}

size_t CombatWorld::load_population_names(const std::string& path) {
    instance_names_.clear();
    asset_names_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;   // (no file: the creatures keep their Blueprint's name)
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (header) { header = false; continue; }
        const size_t first = line.find('\t');
        const size_t second = first == std::string::npos ? first : line.find('\t', first + 1);
        const size_t third = second == std::string::npos ? second : line.find('\t', second + 1);
        if (third == std::string::npos) continue;   // (kind, source, member, text)
        const std::string source_text = line.substr(first + 1, second - first - 1);
        char* end = nullptr;
        const long long source = std::strtoll(source_text.c_str(), &end, 10);
        if (source_text.empty() || *end != '\0') continue;
        // (the lists of an asset set need the set the spawn point drew its asset from, which a spawn does not keep: they are not read)
        const std::string kind = line.substr(0, first);
        if (kind == "instance") instance_names_[source].push_back(line.substr(third + 1));
        else if (kind == "asset") asset_names_[source].push_back(line.substr(third + 1));
    }
    return instance_names_.size() + asset_names_.size();
}

namespace {

/// The names of one list, each once and in the file's order; a blank entry is no name.  "" when the list names nothing.
std::string joined_names(const std::vector<std::string>& list) {
    std::vector<const std::string*> said;
    std::string out;
    for (const std::string& text : list) {
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        bool again = false;
        for (const std::string* before : said) again = again || *before == text;
        if (again) continue;
        said.push_back(&text);
        if (!out.empty()) out += " / ";
        out += text;
    }
    return out;
}

}  // namespace

std::string CombatWorld::shown_name(uint64_t actor_id, const std::string& archetype, const std::string& leaf) const {
    if (const world::WorldActor* a = registry_.find(actor_id)) {
        if (!a->npc.name.empty() && a->npc.name != leaf) return a->npc.name;   // a person the data names has its name on its spawn
        // the population data's name: the first list that has entries (the instance's, else the asset's); the game picks one name of it, a blank pick leaves the Blueprint's name
        const std::vector<std::string>* list = nullptr;
        if (a->npc.instance_guid != 0) {
            const auto it = instance_names_.find(a->npc.instance_guid);
            if (it != instance_names_.end() && !it->second.empty()) list = &it->second;
        }
        if (!list && a->npc.asset_guid != 0) {
            const auto it = asset_names_.find(a->npc.asset_guid);
            if (it != asset_names_.end() && !it->second.empty()) list = &it->second;
        }
        if (list) {
            const std::string names = joined_names(*list);
            if (!names.empty()) return names;
        }
    }
    return display_name(archetype);
}

std::string CombatWorld::log_name(uint64_t actor_id, const std::string& archetype, const std::string& leaf) const {
    const std::string shown = shown_name(actor_id, archetype, leaf);
    return shown.empty() || shown == leaf ? leaf : shown + " (" + leaf + ")";
}

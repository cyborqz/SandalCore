// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  foliage/foliage.cpp (see foliage.h)
// ============================================================================
#include "foliage/foliage.h"

#include "gather/rules.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace aoc::worldd::foliage {

std::unique_ptr<Foliage> Foliage::make(const content::ContentDb& db, gather::Gathering& gathering, world::ActorRegistry& registry, FoliageCells::Minter mint, Config config,
                                       std::string& error) {
    error.clear();
    const content::FoliageData* data = gathering.foliage_data();
    if (!data) {
        error = "content.db has no foliage tables (rebuild it with contentc 15 or later)";
        return nullptr;
    }
    (void)db;
    auto map = FoliageMap::load(config.map_path, error);
    if (!map) {
        error = (error.empty() ? std::string("the foliage map does not load") : error) + " (" + config.map_path + "; it is written by `mapc extract` and belongs in dist\\content)";
        return nullptr;
    }
    std::unique_ptr<Foliage> f(new Foliage(std::move(*map), config));
    const std::vector<std::string>& names = f->map_.def_names();
    f->defs_.resize(names.size());
    std::vector<DefTimes> times(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        DefInfo& d = f->defs_[i];
        d.name = names[i];
        const auto range = data->defs_by_name.equal_range(names[i]);
        if (range.first == range.second) {
            spdlog::warn("[foliage] the map's def {} is no foliage_def: its instances cannot be harvested", names[i]);
            continue;
        }
        int64_t guid = range.first->second;
        size_t records = 0;
        for (auto it = range.first; it != range.second; ++it, ++records) guid = std::min(guid, it->second);   // several records of one name: the first by guid
        if (records > 1) {
            ++f->duplicated_;
            spdlog::info("[foliage] the def {} has {} records in the design data: the first by guid ({}) is used", names[i], records, guid);
        }
        d.row = &data->defs.at(guid);
        DefTimes& t = times[i];
        t.paint = d.row->paint_state;
        t.empty_seconds = d.row->empty_duration;
        for (int s = content::kFoliageInfancy; s <= content::kFoliageAdult; ++s) {
            const int64_t state_guid = d.row->state_guids[s];
            if (state_guid == 0) continue;
            const auto st = data->states.find(state_guid);
            if (st == data->states.end()) continue;
            d.states[s] = &st->second;
            t.has[s] = true;
            t.life[s] = st->second.lifecycle_duration;
            if (st->second.interaction_guid != 0) {
                d.interactions[s] = gathering.interaction(st->second.interaction_guid);
                d.harvestable = d.harvestable || d.interactions[s] != nullptr;
            }
        }
    }
    std::vector<uint32_t> def_of_row(f->map_.rows().size());
    for (size_t r = 0; r < def_of_row.size(); ++r) def_of_row[r] = f->map_.rows()[r].def;
    FoliageStore::Config sc;
    sc.regrow_mult = config.regrow_mult;
    f->store_ = std::make_unique<FoliageStore>(std::move(def_of_row), std::move(times), sc);
    f->cells_ = std::make_unique<FoliageCells>(registry, f->map_, std::move(mint));
    if (config.all_cells) {
        const auto t0 = std::chrono::steady_clock::now();
        const size_t made = f->cells_->make_all();
        spdlog::info("[foliage] {} state actors made, one for every cell of the map ({} components, each initialized): the client finds no tree, plant or stone without them ({:.0f} ms)", made,
                     f->map_.components(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return f;
}
// C | Y | B.CoA

size_t Foliage::harvestable_defs() const {
    size_t n = 0;
    for (const DefInfo& d : defs_) n += d.harvestable ? 1 : 0;
    return n;
}

size_t Foliage::unresolved_defs() const {
    size_t n = 0;
    for (const DefInfo& d : defs_) n += d.row ? 0 : 1;
    return n;
}

double Foliage::reach_cm(uint32_t row, int state) const {
    if (row >= map_.rows().size() || state < content::kFoliageInfancy || state > content::kFoliageAdult) return 0.0;
    const DefInfo& d = def_of(row);
    if (!d.states[state] || !d.interactions[state]) return 0.0;
    const content::FoliageStateRow& st = *d.states[state];
    const content::GatherDefRow& it = *d.interactions[state];
    return std::max(st.override_distance, it.interact_radius) + it.interact_tolerance + st.collision_radius + gather::kLeewayCm;
}

size_t Foliage::tick(Clock::time_point now) {
    const auto due = store_->next_due();
    const double t = seconds(now);
    cells_->set_now(t);
    if (!due || *due > t) return 0;
    const std::vector<Change> changes = store_->advance(t);
    if (!changes.empty()) cells_->apply(changes);
    for (const Change& c : changes) {
        const FoliageMap::Row& r = map_.rows()[c.row];
        const char* what = c.state >= kInfancy && c.state <= kEmpty ? kStateNames[c.state] : "?";
        spdlog::log(logged_++ < 60 ? spdlog::level::info : spdlog::level::debug, "[foliage] {} ({} {} #{}) grows: now {}{}", name_of(c.row), map_.owners()[r.owner].name,
                    map_.owners()[r.owner].components[r.component], r.index, what, c.painted ? ", whole again" : "");
    }
    return changes.size();
}

std::optional<Change> Foliage::harvested(uint32_t row, Clock::time_point now) {
    cells_->set_now(seconds(now));
    const auto c = store_->harvest(row, seconds(now));
    if (c) cells_->apply({*c});
    return c;
}

std::string Foliage::describe() const {
    char buf[320];
    std::snprintf(buf, sizeof buf, "%s; %zu defs, %zu harvestable, %zu unresolved; %zu cut or growing, %zu state actors; regrowth x%g", map_.describe().c_str(), defs_.size(),
                  harvestable_defs(), unresolved_defs(), store_->deviations(), cells_->actors(), config_.regrow_mult);
    return buf;
}

}  // namespace aoc::worldd::foliage

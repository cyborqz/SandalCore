// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  foliage/foliage_map.cpp (see foliage_map.h)
// ============================================================================
#include "foliage/foliage_map.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>

namespace aoc::worldd::foliage {

namespace {

/// The columns of region_foliage.tsv v2.
constexpr size_t kColumns = 17;
constexpr size_t kColDefName = 1, kColOwner = 3, kColComponent = 4, kColIndex = 5, kColX = 6, kColY = 7, kColZ = 8;

template <class T>
bool number(std::string_view s, T& out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

/// "InstancedFoliageActor_25600_<cx>_<cy>_<n>" -> the cell; false for any other name.
bool cell_of(std::string_view name, int& cx, int& cy) {
    constexpr std::string_view kPrefix = "InstancedFoliageActor_25600_";
    if (name.substr(0, kPrefix.size()) != kPrefix) return false;
    name.remove_prefix(kPrefix.size());
    const size_t a = name.find('_');
    const size_t b = a == std::string_view::npos ? a : name.find('_', a + 1);
    if (a == std::string_view::npos || b == std::string_view::npos) return false;
    return number(name.substr(0, a), cx) && number(name.substr(a + 1, b - a - 1), cy);
}

struct Raw {
    uint32_t def = 0;
    uint32_t owner = 0;
    std::string_view component;
    uint32_t index = 0;
    float x = 0, y = 0, z = 0;
    int64_t z_tenths = 0;   // z as the file writes it (one decimal), for the owner's lowest and highest
};
// == CoA | C | Y | B ==

}  // namespace

std::optional<FoliageMap> FoliageMap::load(const std::string& path, std::string& error) {
    error.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse(text, error);
}

std::optional<FoliageMap> FoliageMap::parse(std::string_view text, std::string& error) {
    error.clear();
    FoliageMap m;
    std::unordered_map<std::string, uint32_t> def_index;
    std::vector<Raw> raw;
    std::vector<std::map<std::string_view, int>> comps;   // per owner: the component names it has (sorted below)
    raw.reserve(90000);
    size_t line_no = 0, at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        std::string_view col[kColumns];
        size_t n = 0, from = 0;
        while (n < kColumns) {
            const size_t t = line.find('\t', from);
            if (t == std::string_view::npos) {
                col[n++] = line.substr(from);
                break;
            }
            col[n++] = line.substr(from, t - from);
            from = t + 1;
            if (n == kColumns - 1) {   // the last column (tags) may be empty and is not read
                col[n++] = line.substr(from);
                break;
            }
        }
        const auto fail = [&](const std::string& why) {
            error = "region_foliage.tsv line " + std::to_string(line_no) + ": " + why;
            return std::optional<FoliageMap>{};
        };
        if (n != kColumns) return fail(std::to_string(n) + " columns, want " + std::to_string(kColumns));
        Raw r;
        double x = 0, y = 0, z = 0;
        if (!number(col[kColIndex], r.index)) return fail("the index is no number: " + std::string(col[kColIndex]));
        if (!number(col[kColX], x) || !number(col[kColY], y) || !number(col[kColZ], z) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return fail("the position is no number");
        }
        r.x = static_cast<float>(x);
        r.y = static_cast<float>(y);
        r.z = static_cast<float>(z);
        r.z_tenths = std::llround(z * 10.0);
        if (col[kColDefName].empty() || col[kColOwner].empty() || col[kColComponent].empty()) return fail("a def name, owner or component is empty");
        const std::string def_name(col[kColDefName]);
        const auto d = def_index.emplace(def_name, static_cast<uint32_t>(m.def_names_.size()));
        if (d.second) m.def_names_.push_back(def_name);
        r.def = d.first->second;
        const std::string owner_name(col[kColOwner]);
        auto o = m.owner_by_name_.find(owner_name);
        if (o == m.owner_by_name_.end()) {
            if (m.owners_.size() >= 65535) return fail("more owners than the 16-bit index holds");
            Owner owner;
            owner.name = owner_name;
            if (!cell_of(owner_name, owner.cell_x, owner.cell_y)) return fail("the owner " + owner_name + " is no InstancedFoliageActor_25600_<cx>_<cy>_<n>");
            o = m.owner_by_name_.emplace(owner_name, static_cast<uint16_t>(m.owners_.size())).first;
            m.owners_.push_back(std::move(owner));
            comps.emplace_back();
        }
        r.owner = o->second;
        r.component = col[kColComponent];   // (into `text`, which outlives this function's use of it)
        comps[r.owner].emplace(r.component, 0);
        raw.push_back(r);
    }
    if (raw.empty()) {
        error = "region_foliage.tsv holds no instance";
        return std::nullopt;
    }
    // the components of each owner, sorted by name
    for (size_t o = 0; o < m.owners_.size(); ++o) {
        Owner& owner = m.owners_[o];
        int i = 0;
        for (auto& [name, idx] : comps[o]) {
            if (i >= 65535) {
                error = "more components in " + owner.name + " than the 16-bit index holds";
                return std::nullopt;
            }
            idx = i++;
            owner.components.emplace_back(name);
        }
        m.components_ += owner.components.size();
    }
    m.rows_.reserve(raw.size());
    m.row_by_key_.reserve(raw.size() * 2);
    std::vector<bool> z_seen(m.owners_.size(), false);
    for (size_t i = 0; i < raw.size(); ++i) {
        const Raw& r = raw[i];
        Row row;
        row.def = r.def;
        row.owner = static_cast<uint16_t>(r.owner);
        row.component = static_cast<uint16_t>(comps[r.owner].at(r.component));
        row.index = r.index;
        row.x = r.x;
        row.y = r.y;
        row.z = r.z;
        const uint64_t key = key_of(row.owner, row.component, row.index);
        if (!m.row_by_key_.emplace(key, static_cast<uint32_t>(i)).second) {
            error = "region_foliage.tsv: the instance (" + m.owners_[row.owner].name + ", " + m.owners_[row.owner].components[row.component] + ", " + std::to_string(row.index) +
                    ") is listed twice";
            return std::nullopt;
        }
        Owner& owner = m.owners_[row.owner];
        owner.rows.push_back(static_cast<uint32_t>(i));
        owner.z_lo_tenths = z_seen[row.owner] ? std::min(owner.z_lo_tenths, r.z_tenths) : r.z_tenths;
        owner.z_hi_tenths = z_seen[row.owner] ? std::max(owner.z_hi_tenths, r.z_tenths) : r.z_tenths;
        z_seen[row.owner] = true;
        m.rows_.push_back(row);
    }
    return m;
}

std::optional<uint16_t> FoliageMap::owner_of(std::string_view name) const {
    const auto it = owner_by_name_.find(std::string(name));
    if (it == owner_by_name_.end()) return std::nullopt;
    return it->second;
}

std::optional<uint32_t> FoliageMap::find(std::string_view owner, std::string_view component, uint32_t index) const {
    const auto o = owner_of(owner);
    if (!o) return std::nullopt;
    const std::vector<std::string>& comps = owners_[*o].components;
    const auto c = std::lower_bound(comps.begin(), comps.end(), component);
    if (c == comps.end() || *c != component) return std::nullopt;
    const auto it = row_by_key_.find(key_of(*o, static_cast<uint16_t>(c - comps.begin()), index));
    if (it == row_by_key_.end()) return std::nullopt;
    return it->second;
}

void FoliageMap::cell_center(uint16_t owner, double& x, double& y) const {
    const Owner& o = owners_[owner];
    x = (static_cast<double>(o.cell_x) + 0.5) * kCellCm;
    y = (static_cast<double>(o.cell_y) + 0.5) * kCellCm;
}

std::string FoliageMap::describe() const {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%zu instances of %zu defs in %zu owners with %zu components", rows_.size(), def_names_.size(), owners_.size(), components_);
    return buf;
}

}  // namespace aoc::worldd::foliage

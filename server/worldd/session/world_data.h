// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/world_data.h
//
//  What a PlayerWorld reads once from content.db and never writes again: the
//  ability rules, the item and gear catalogs, the stat program, the leveling
//  curve and its ranks, the skill trees, the professions, the weapon lines, the
//  currency tiers, the ability tables and the mount tables.  The production
//  constructor builds its own object; a test program may hold one object per
//  database file and build every fresh world from it.
// ============================================================================
#pragma once

#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "items/currency.h"
#include "items/inventory.h"
#include "session/ability_catalog.h"
#include "session/leveling.h"
#include "session/professions.h"
#include "session/talents.h"
#include "session/weapon_lines.h"
#include "spawns/mount_spawn.h"
#include "stats/stat_program.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

class PlayerWorld;

class WorldData {
public:
    ~WorldData();

    /// Reads every data set of `content` once.  `problem` says why the world
    /// would not be ready (empty when it is).  A null content gives no object.
    static std::shared_ptr<const WorldData> load(const content::ContentDb* content, std::string& problem);

    /// How many WorldData objects this program has built, and how many
    /// different database files they came from (the tests read both).
    static size_t loads();
    static size_t files();

    bool ready() const { return content_ != nullptr; }
    const content::ContentDb* content() const { return content_; }
    const std::string& problem() const { return problem_; }

    const content::AbilityRules* ability_rules() const { return rules_ ? &*rules_ : nullptr; }
    const items::ItemCatalog* item_catalog() const { return catalog_ ? &*catalog_ : nullptr; }
    const stats::StatBundle* stat_bundle() const { return bundle_ ? &*bundle_ : nullptr; }
    const content::ContentGearCatalog* gear_catalog() const { return gear_ ? &*gear_ : nullptr; }
    const leveling::LevelCurve* level_curve() const { return curve_ ? &*curve_ : nullptr; }
    const std::vector<content::AutoGrant>& auto_grants() const { return grants_; }
    const content::LevelingType* leveling_type() const { return leveling_type_ ? &*leveling_type_ : nullptr; }
    const talents::Catalog* talents() const { return talents_ ? &*talents_ : nullptr; }
    const professions::Book* professions() const { return professions_ ? &*professions_ : nullptr; }
    const weaponxp::Lines* weapon_lines() const { return weapon_lines_ ? &*weapon_lines_ : nullptr; }
    const items::CurrencyTable* currencies() const { return currencies_ ? &*currencies_ : nullptr; }
    const abilities::Catalog* ability_catalog() const { return ability_catalog_ ? &*ability_catalog_ : nullptr; }
    const spawns::MountBook* mounts() const { return mounts_ ? &*mounts_ : nullptr; }
    bootstrap::StatContext stat_context() const;

private:
    explicit WorldData(const content::ContentDb* content);

    const content::ContentDb*                content_ = nullptr;
    std::string                              problem_;
    std::optional<content::AbilityRules>     rules_;
    std::optional<items::ContentItemCatalog> catalog_;
    std::optional<stats::StatBundle>         bundle_;
    std::optional<content::ContentGearCatalog> gear_;
    std::optional<content::LevelingType>     leveling_type_;
    std::optional<leveling::LevelCurve>      curve_;
    std::vector<content::AutoGrant>          grants_;
    std::optional<talents::Catalog>          talents_;
    std::optional<professions::Book>         professions_;
    std::optional<weaponxp::Lines>           weapon_lines_;
    std::optional<items::CurrencyTable>      currencies_;
    std::optional<abilities::Catalog>        ability_catalog_;
    std::optional<spawns::MountBook>         mounts_;
};

}}  // namespace aoc::worldd

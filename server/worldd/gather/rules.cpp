// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  gather/rules.cpp (see rules.h)
// ============================================================================
#include "gather/rules.h"

#include <string>

namespace aoc::worldd::gather {

const char* refusal_name(Refusal r) {
    switch (r) {
        case Refusal::None: return "none";
        case Refusal::NoProfessions: return "no professions";
        case Refusal::NotCertified: return "not certified";
        case Refusal::LevelTooLow: return "level too low";
        case Refusal::NoTool: return "no tool";
        case Refusal::NotATool: return "not a tool";
        case Refusal::WrongToolProfession: return "wrong tool";
        case Refusal::ToolTierTooLow: return "tool tier too low";
        case Refusal::ToolBroken: return "tool broken";
        case Refusal::ToolNotUsable: return "tool not usable";
        case Refusal::InCombat: return "in combat";
        case Refusal::kCount: break;
    }
    return "?";
}

namespace {

Verdict refuse(Refusal why, std::string text) {
    Verdict v;
    v.why = why;
    v.text = std::move(text);
    return v;
}

std::string profession_name(const professions::Professions& p, int64_t guid) {
    const professions::Profession* pf = p.valid() ? p.book()->find(guid) : nullptr;
    return pf ? pf->name : std::to_string(guid);
}

}  // namespace
// (C-O-A ~ Cyb)

Verdict check_requirements(const content::GatherDefRow& def, const professions::Professions& p, const ToolInfo& tool, bool in_combat) {
    if (!p.valid()) return refuse(Refusal::NoProfessions, "the world has no profession tables");
    const professions::Book& book = *p.book();
    const professions::Tier& need = book.tier(def.cert_tier);
    const professions::State have = p.state(def.profession_guid);
    if (have.tier < def.cert_tier) {
        return refuse(Refusal::NotCertified, profession_name(p, def.profession_guid) + " is certified at " + book.tier(have.tier).name + ", the node needs " + need.name);
    }
    if (have.level < need.min_level) {
        return refuse(Refusal::LevelTooLow, profession_name(p, def.profession_guid) + " is level " + std::to_string(have.level) + ", the node needs level " + std::to_string(need.min_level));
    }
    if (!tool.worn) return refuse(Refusal::NoTool, "nothing is worn in the tool slot " + std::to_string(def.tool_slot));
    if (!tool.tool) return refuse(Refusal::NotATool, "the item in the tool slot " + std::to_string(def.tool_slot) + " is no gathering tool");
    if (tool.tool->profession_guid != def.profession_guid) {
        return refuse(Refusal::WrongToolProfession, "the tool " + tool.tool->item_name + " is for " + profession_name(p, tool.tool->profession_guid) + ", not " + profession_name(p, def.profession_guid));
    }
    if (tool.tool->cert_tier < def.cert_tier) {
        return refuse(Refusal::ToolTierTooLow, "the tool " + tool.tool->item_name + " reaches " + book.tier(tool.tool->cert_tier).name + " nodes, this one is " + need.name);
    }
    if (!(tool.durability > 0)) return refuse(Refusal::ToolBroken, "the tool " + tool.tool->item_name + " has no durability left");
    // the character may use the tool: its own requirement, else its tool data's tier (the T3 tools need Journeyman)
    const int64_t req_profession = tool.tool->required_profession != 0 ? tool.tool->required_profession : tool.tool->profession_guid;
    int req_tier = tool.tool->cert_tier;
    if (tool.tool->required_cert != 0) {
        const int t = book.tier_of_guid(tool.tool->required_cert);
        if (t >= 0) req_tier = t;
    }
    const professions::State tool_have = p.state(req_profession);
    if (tool_have.tier < req_tier || tool_have.level < book.tier(req_tier).min_level) {
        return refuse(Refusal::ToolNotUsable, "the tool " + tool.tool->item_name + " needs " + profession_name(p, req_profession) + " certified at " + book.tier(req_tier).name + ", the character is " +
                                                 book.tier(tool_have.tier).name + " level " + std::to_string(tool_have.level));
    }
    if (in_combat && !def.allow_in_combat) return refuse(Refusal::InCombat, "this node cannot be harvested in combat");
    return {};
}

}  // namespace aoc::worldd::gather

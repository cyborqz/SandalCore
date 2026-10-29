// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/session_givers.inc  (quests milestone, "map markers")
//
//  PlayerSession's members for the map: the pins of the objectives that are active (the log is brought in line with the quest component in quest_sync, which makes the items) and the
//  quest givers (the NPCs that have a quest the character can take).  Included at the end of session/player_session.cpp, inside namespace aoc::worldd.
//
//  A quest giver is told to the client as an item of the quest component's QuestGiverInfos array: the NPC's population instance, the flavor of the quest on offer and the map
//  registration the quest names for its giver.  The client puts that registration on the NPC (its quest-available mark, and its place on the map).  An NPC that has nothing on offer
//  any more keeps its item, changed to "none": the client keeps what it was told when an item is deleted.  The client adds each registration it is told to the ones the NPC shows and only
//  "none" empties them, so an NPC goes from one registration to another through "none".  Only the NPCs the client can have are looked at (the relevancy rings).
// ============================================================================

namespace {

/// How far from the character the NPCs are looked at: the outer ring of the relevancy rings (an NPC beyond it is not on the client), with its hysteresis.
constexpr double kGiverRadiusCm = 36100.0;
/// How often the givers are looked at when the log did not change (the character's level, items and place change what is on offer too).
constexpr std::chrono::seconds kGiversEvery{5};
/// The most givers one character is told of (the Riverlands have about 310 NPCs that offer a quest).
constexpr size_t kMaxGivers = 600;
/// The most NPCs one update adds (the original server told 4 to 12 at a time); the rest follow with the next look, a tick later.
constexpr size_t kMaxNewGiversPerLook = 24;

}  // namespace

void PlayerSession::log_quest_markers(const quest::MarkerContext& markers) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (markers.refused > 0) spdlog::warn("[session] {}: {} objective(s) have places to mark and no pin: they do not fit the layout of the quest component's marker array", name(), markers.refused);
    for (const quest::MarkerContext::Made& m : markers.made) {
        std::string objective = std::to_string(m.objective), quest_name = std::to_string(m.quest);
        if (const content::QuestRow* d = qb ? qb->quest(m.quest) : nullptr) {
            quest_name = d->name;
            for (const content::QuestObjectiveRow& o : d->objectives) {
                if (o.guid == m.objective) objective = o.name;
            }
        }
        spdlog::log(level_for(quests_logged_, 300), "[session] {}: map pin for the objective {} of quest {}: {} place(s), the first {} at ({:.0f}, {:.0f}){}", name(), objective, quest_name, m.places,
                    m.area ? "a circle" : "a pin", m.x, m.y, m.area ? ", radius " + std::to_string(static_cast<long long>(m.radius / 100.0)) + " m" : std::string());
    }
}

void PlayerSession::tick_quest_givers(Connection& conn, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    if (!quests_on_ || !qb || !book || !registry_ || !phase_in_done_ || dead_) return;
    if (!givers_dirty_ && now - givers_checked_ < kGiversEvery) return;
    const bool log_changed = givers_dirty_;   // (then every giver the client was told of is looked at again; else only the ones around)
    givers_dirty_ = false;
    givers_checked_ = now;
    repl::FastArrays* arrays = pb_.quest_arrays();
    const protocol::schema::ClassSchema* cls = quest::component_schema();
    repl::FastArrayState* array = arrays && cls ? arrays->get(*cls, "QuestGiverInfos") : nullptr;
    if (!array) return;
    // the NPCs the client can have that offer a quest, and, when the log changed, the ones it was told of before (what they offer may have changed)
    struct Candidate {
        int64_t     instance = 0, asset = 0;
        std::string name;
    };
    std::vector<Candidate> candidates;
    {
        const GoodPosition at = checker_.good();
        std::vector<uint64_t> near_ids;
        registry_->query(at.x, at.y, kGiverRadiusCm, near_ids);
        std::set<int64_t> seen;
        for (const uint64_t id : near_ids) {
            const world::WorldActor* a = registry_->find(id);
            if (!a || a->kind != world::ActorKind::Npc || a->npc.instance_guid == 0) continue;
            if (qb->offers(a->npc.instance_guid, a->npc.asset_guid).empty() || !seen.insert(a->npc.instance_guid).second) continue;
            candidates.push_back({a->npc.instance_guid, a->npc.asset_guid, a->npc.name});
        }
        if (log_changed) {
            for (const auto& [instance, e] : givers_) {
                if (seen.insert(instance).second) candidates.push_back({instance, e.asset, e.name});
            }
        }
    }
    if (candidates.empty()) return;
    const auto facts = dialogue_facts(nullptr);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a quest could not be answered and does not hold: {} (told once)", name(), what);
    });
    bool changed = false;
    size_t added = 0;
    for (const Candidate& c : candidates) {
        // the first quest on offer that the character can take decides what the NPC shows
        int64_t registration = 0;
        uint8_t flavor = quest::kGiverFlavorNone;
        const content::QuestRow* chosen = nullptr;
        for (const content::QuestOfferRow& offer : qb->offers(c.instance, c.asset)) {
            const content::QuestRow* def = qb->quest(offer.quest);
            if (!def || def->giver_registration == 0) continue;
            if (quest_log_.can_start(def, ev, /*bypass_rules=*/false) != quest::Create::Ok) continue;
            registration = def->giver_registration;
            flavor = quest::giver_flavor(def->flavor);
            chosen = def;
            break;
        }
        const auto it = givers_.find(c.instance);
        if (it == givers_.end()) {
            if (registration == 0) continue;   // nothing on offer and the client was told nothing: it has nothing to forget
            if (givers_.size() >= kMaxGivers) {
                if (!givers_told_) spdlog::warn("[session] {}: the client was told of {} quest givers: no more are added", name(), givers_.size());
                givers_told_ = true;
                continue;
            }
            if (added >= kMaxNewGiversPerLook) {
                givers_dirty_ = true;   // (the others come with the next look)
                continue;
            }
            const auto item = quest::giver_item(array->layout(), c.instance, flavor, registration);
            const auto id = item ? array->add(*item) : std::nullopt;
            if (!id) {
                spdlog::warn("[session] {}: a quest giver does not fit the layout of the quest component's giver array", name());
                return;
            }
            ++added;
            givers_[c.instance] = {*id, registration, flavor, c.asset, c.name};
            spdlog::log(level_for(quests_logged_, 300), "[session] {}: {} has a quest to take ({}): the client is told of its mark", name(), c.name, chosen ? chosen->name : std::string("?"));
            changed = true;
            continue;
        }
        GiverEntry& e = it->second;
        if (e.registration == registration && e.flavor == flavor) continue;
        // the client ADDS the registration of an update to the ones the NPC shows (the last one added wins, one that is there already changes nothing) and only "none" empties them: a change
        // from one registration to another goes through "none", one update apart, so that a registration that comes back is not left behind the one that came after it
        const bool via_none = e.registration != 0 && registration != 0;
        const int64_t send_registration = via_none ? 0 : registration;
        const uint8_t send_flavor = via_none ? quest::kGiverFlavorNone : flavor;
        const auto item = quest::giver_item(array->layout(), c.instance, send_flavor, send_registration);
        if (!item || !array->change(e.id, *item)) {
            spdlog::warn("[session] {}: a quest giver's item cannot be changed", name());
            return;
        }
        e.registration = send_registration;
        e.flavor = send_flavor;
        if (via_none) givers_dirty_ = true;   // (the registration itself follows with the next look, a tick later)
        spdlog::log(level_for(quests_logged_, 300), "[session] {}: {} {}", name(), c.name,
                    via_none ? std::string("goes from one quest-available mark to another (through none)") : chosen ? "has a quest to take (" + chosen->name + "): the mark changes" : std::string("has no quest to take any more: the mark is taken off"));
        changed = true;
    }
    if (changed) queue("QuestConsumerComponent", [](bootstrap::PlayerBootstrap& pb) { return pb.quest_update(); }, conn, /*must_build=*/false);
}

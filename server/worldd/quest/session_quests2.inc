// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/session_quests2.inc  (quests milestone, slice 2, "do it and hand it in")
//
//  PlayerSession's members for playing a quest, included at the end of session/player_session.cpp (inside namespace aoc::worldd): the quest engine (quest/engine.h) is built over the
//  character's log whenever something happens that a quest may count (a conversation action, a kill, an interaction, the place the character stands at, the items it carries, a delivery
//  of crates) and says what the world has to do: give the rewards, take items, show a toast, take the finished quest out of the log.  This file does those things.
// ============================================================================

namespace {

/// How often the places, the items carried and the rules are looked at.
constexpr std::chrono::seconds kQuestTick{1};

}  // namespace

double PlayerSession::quest_clock(Clock::time_point now) { return pb_.setup().world_time + session_seconds(now); }

void PlayerSession::quest_engine(Connection& conn, Clock::time_point now, const char* why, const std::function<void(quest::Engine&)>& fn) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    if (!quests_on_ || !qb || !book) return;
    quest_last_now_ = now;
    const auto facts = dialogue_facts(nullptr);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a quest could not be answered and does not hold: {} (told once)", name(), what);
    });
    std::vector<quest::Effect> effects;
    quest::Engine engine(quest_log_, *qb, ev, *facts, effects, quest_clock(now));
    fn(engine);
    apply_quest_effects(conn, effects, now);
    if (quest_log_.wire_pending()) quest_sync(conn, why);
}

void PlayerSession::apply_quest_effects(Connection& conn, std::vector<quest::Effect>& effects, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const auto quest_name = [&](int64_t record) {
        const content::QuestRow* d = qb ? qb->quest(record) : nullptr;
        return d ? d->name : std::to_string(record);
    };
    for (const quest::Effect& e : effects) {
        switch (e.kind) {
            case quest::Effect::Kind::GrantTable:
                grant_quest_rewards(conn, e.tables, "a quest action", now, e.quest);
                break;
            case quest::Effect::Kind::GrantRewards:
                grant_quest_rewards(conn, e.tables, "a finished quest", now, e.quest);
                break;
            case quest::Effect::Kind::RemoveItems:
                take_quest_items(conn, e.items);
                break;
            case quest::Effect::Kind::Toast:
                quest_toast(conn, e.text, now);
                break;
            case quest::Effect::Kind::ObjectiveDone: {
                ++quest_objectives_done_;
                std::string objective = std::to_string(e.objective);
                if (const content::QuestRow* d = qb ? qb->quest(e.quest) : nullptr) {
                    for (const content::QuestObjectiveRow& o : d->objectives) {
                        if (o.guid == e.objective) objective = o.name;
                    }
                }
                spdlog::log(level_for(quests_logged_, 200), "[session] {}: finished the objective {} of quest {}", name(), objective, quest_name(e.quest));
                break;
            }
            case quest::Effect::Kind::QuestDone:   // (the quest leaves the log a moment later, tick_quests: the client is told it is done first)
                ++quests_finished_;
                spdlog::log(level_for(quests_logged_, 200), "[session] {}: finished quest {}", name(), quest_name(e.quest));
                break;
            case quest::Effect::Kind::QuestFailed:
                spdlog::log(level_for(quests_logged_, 200), "[session] {}: failed quest {}", name(), quest_name(e.quest));
                break;
            case quest::Effect::Kind::StartQuest: {
                const content::QuestRow* def = qb ? qb->quest(e.quest) : nullptr;
                const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
                if (!def || !book) break;
                const auto facts = dialogue_facts(nullptr);
                dialogue::Evaluator ev(*book, *facts);
                const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/false);
                if (c == quest::Create::Ok) start_quest(conn, *def, 0, "a quest action started it", now);
                else spdlog::log(level_for(quests_logged_, 200), "[session] {}: a quest action would start quest {}: not taken, {}", name(), def->name, quest::create_name(c));
                break;
            }
            case quest::Effect::Kind::Told:
                if (quest_told_.insert(e.text).second) spdlog::info("[session] {}: {} (told once)", name(), e.text);
                break;
        }
    }
}

// ---- rewards ---------------------------------------------------------------------------------------------------------------------------------------------------

void PlayerSession::grant_quest_rewards(Connection& conn, const std::vector<int64_t>& tables, const char* why, Clock::time_point now, int64_t quest) {
    if (tables.empty()) return;
    // behind rewards that wait already (they are given in order)
    if (!quest_rewards_waiting_.empty() || !deliver_quest_rewards(conn, tables, why, now, quest)) quest_rewards_waiting_.push_back({tables, why, quest});
}

bool PlayerSession::deliver_quest_rewards(Connection& conn, const std::vector<int64_t>& tables, const char* why, Clock::time_point now, int64_t quest) {
    const combat::RewardBook* book = world_ ? world_->rewards() : nullptr;
    if (!book) {
        if (quest_told_.insert("no reward tables").second) spdlog::warn("[session] {}: the reward tables of {} are not given: this world has no reward tables", name(), why);
        return true;
    }
    if (!room_for(16 + world_->auto_grants().size())) return false;   // (the frames of the reward wait for room in the reply queue)
    std::string err;
    auto run = gather_run(err);
    if (!run) {
        spdlog::warn("[session] {}: the reward tables of {} are not given: no stat run for the rolls ({})", name(), why, err);
        return true;
    }
    combat::RollContext ctx;
    ctx.npc = run.get();   // the expressions run as the character
    ctx.player_level = progress_.level;
    ctx.loot_max_level = progress_.level;
    if (const quest::QuestBook* qb = world_->quests()) {   // the experience and the Glint of a quest follow its level (GetNarrativeContentLevel)
        if (const content::QuestRow* def = qb->quest(quest)) ctx.narrative_level = def->level;
    }
    ctx.rng = &quest_rng_;
    const GoodPosition at = checker_.good();
    const world::CityNodes* nodes = world_->city_nodes();
    if (nodes) {
        const world::NodeSite site = nodes->node_at(at.x, at.y);
        ctx.nodes = nodes;
        ctx.node = site.node_rank;
        ctx.biome = site.biome;
    }
    const combat::Rolled rolled = book->roll(tables, ctx);
    gather::Harvest h;
    h.durability_loss = 0;
    h.items = rolled.loot;
    h.items.insert(h.items.end(), rolled.direct.begin(), rolled.direct.end());
    // a quest-bound crate is carried, not stored (ONE call places it)
    if (const quest::QuestBook* qb = world_->quests()) {
        for (auto it = h.items.begin(); it != h.items.end();) {
            if (qb->crate_of(it->item) != 0 && qb->item_links(it->item) && carry_quest_crate(conn, *it, now)) it = h.items.erase(it);
            else ++it;
        }
    }
    int64_t node_xp = 0, weapon_xp = 0;
    for (const combat::RolledXp& x : rolled.xp) {
        if (x.type == "Adventuring") h.adventuring_xp += x.amount;
        else if (x.type == "NodeActivity") node_xp += x.amount;
        else if (x.type == "Weapon") weapon_xp += x.amount;
    }
    h.tables = rolled.tables;
    h.gated = rolled.gated;
    HarvestFrame frame;
    frame.what = std::string("the rewards of ") + why;
    frame.harvest = &h;
    frame.tag = "quest";
    deliver_harvest(conn, now, frame);
    ++quest_rewards_given_;
    // the town: where the NPC stands that the character talks to (else where the character stands); only a real town (one of the nodes that grow) gets the experience
    std::string node_note;
    if (node_xp > 0) {
        double gx = at.x, gy = at.y;
        if (quest_giver_set_) {
            gx = quest_giver_x_;
            gy = quest_giver_y_;
        }
        const world::CityNodeInfo* town = nullptr;
        if (nodes) town = nodes->by_rank(nodes->node_at(gx, gy).node_rank);
        world::NodeControl* control = world_->node_control();
        if (town && town->levelable && control) {
            control->add_experience(std::to_string(town->guid), node_xp, std::string("a quest reward of ") + name());
            node_note = "; " + std::to_string(node_xp) + " node experience to " + town->display_name;
        } else {
            node_note = "; " + std::to_string(node_xp) + " node experience goes nowhere (" +
                        (!control ? "no node service" : town ? town->display_name + " is not a town that grows" : "the giver stands in no node") + ")";
        }
    }
    size_t held = 0;   // the tables the reward book holds (a table it lacks gives nothing)
    for (const int64_t t : tables) held += book->table(t) ? 1 : 0;
    spdlog::log(level_for(quests_logged_, 200), "[session] {}: quest rewards ({}): {} table(s) named, {} in the reward book, {} rolled with their sub tables, {} item stack(s), Adventuring +{} XP{}{}{}", name(), why, tables.size(), held,
                rolled.tables, h.items.size(), h.adventuring_xp, node_note, weapon_xp > 0 ? ", weapon experience " + std::to_string(weapon_xp) + " not modelled" : "",
                rolled.gated > 0 ? ", " + std::to_string(rolled.gated) + " formula(s) did not run" : "");
    return true;
}

void PlayerSession::take_quest_items(Connection& conn, const std::vector<std::pair<int64_t, int64_t>>& items) {
    items::Outcome all;
    all.ok = true;
    for (const auto& [record, quantity] : items) {
        const int64_t have = inventory_.count_of(record);
        const int64_t take = quantity <= 0 ? have : std::min(quantity, have);
        if (take <= 0) continue;
        items::Outcome o = inventory_.take(record, take);
        if (!o.ok) continue;
        all.events.insert(all.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (all.events.empty()) return;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::info("[session] {}: {} item event(s) took the items a quest asked for", name(), events.size());
}

void PlayerSession::quest_toast(Connection& conn, const std::string& text, Clock::time_point now) {
    constexpr int32_t kCalloutType = 1, kCalloutPointSize = 32;
    constexpr std::chrono::seconds kCalloutLife{3};
    const int64_t ticks = items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const uint64_t key = (static_cast<uint64_t>(pb_.setup().player_id) << 32) | (0x40000000u + gather_callout_seq_++);
    queue("a callout", [text, ticks, key](bootstrap::PlayerBootstrap& pb) { return pb.callout_add(text, kCalloutType, kCalloutPointSize, ticks, key); }, conn);
    gather_callouts_.push_back({now + kCalloutLife, key});
}

// ---- what a quest counts -------------------------------------------------------------------------------------------------------------------------------------------

void PlayerSession::run_talk_actions(Connection& conn, const dialogue::Result& r, const world::WorldActor& npc, Clock::time_point now) {
    if (r.actions.empty() || !quests_on_) return;
    const int64_t instance = npc.npc.instance_guid;
    quest_giver_x_ = npc.x;
    quest_giver_y_ = npc.y;
    quest_giver_set_ = true;
    quest_engine(conn, now, "a conversation", [&](quest::Engine& eng) {
        for (const dialogue::Result::Run& run : r.actions) {
            const std::string& a = run.action.action;
            if (a == "CompleteDialogueObjective" || a == "IncrementDialogueObjective") eng.dialogue(instance, run.visited.empty() ? r.visited : run.visited, a == "CompleteDialogueObjective");
            else eng.dialogue_action(run.action);
        }
    });
    quest_giver_set_ = false;
}

void PlayerSession::quest_kill(Connection& conn, const combat::KillReward& k, Clock::time_point now) {
    if (!quests_on_ || k.instance == 0 || quest_log_.instances().empty()) return;
    quest_engine(conn, now, "a kill", [&](quest::Engine& eng) { eng.kill(k.instance, 0); });
}

void PlayerSession::quest_shrine(Connection& conn, int64_t place, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!quests_on_ || !qb || quest_log_.instances().empty()) return;
    const std::vector<int64_t>& icds = qb->shrine_icds(place);
    if (icds.empty()) return;
    quest_engine(conn, now, "an Emberspring", [&](quest::Engine& eng) {
        for (const int64_t icd : icds) eng.interact(icd, 0);
    });
}

void PlayerSession::construction_crate_equipped(int64_t item_record) {
    if (!quests_on_ || quest_crate_events_.size() >= 64) return;
    CrateEvent e;
    e.item = item_record;
    quest_crate_events_.push_back(e);
}

void PlayerSession::construction_deposit(int64_t node, int64_t building_or_project, int64_t item_record, int32_t count) {
    if (!quests_on_ || count <= 0 || quest_crate_events_.size() >= 64) return;
    CrateEvent e;
    e.deposit = true;
    e.node = node;
    e.project = building_or_project;
    e.item = item_record;
    e.count = count;
    quest_crate_events_.push_back(e);
}

bool PlayerSession::carry_quest_crate(Connection& conn, const combat::RolledItem& crate, Clock::time_point now) {
    // A crate a quest gives goes to the back like a made one (the cargo slot holds ONE crate).  With the slot taken, or for an item that is no carried crate, the caller keeps it among the other
    // rewards (the bag): the reward is not lost.
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || crate.quantity != 1) return false;
    std::string error;
    const auto rec = db->item(crate.item, error);
    if (!rec) return false;
    const auto info = item_catalog().info(crate.item);
    if (!info || !info->cargo_crate()) return false;
    if (inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        say(conn, "You are already carrying a crate.", now);
        spdlog::info("[session] {}: a quest gives the crate {}: a crate is carried already, so it goes into the bag", name(), rec->name);
        return false;
    }
    svc::InventoryItem it;
    it.container = svc::ItemContainer::Equipment;
    it.slot = items::equip::CargoHold;
    it.item_record_id = crate.item;
    it.instance_guid = fresh_guid();
    it.quantity = 1;
    it.quality = crate.quality > 0 ? crate.quality : std::max(1, rec->default_quality);
    it.durability = rec->max_durability_at(it.quality);
    it.created_at_ms = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    const items::Outcome o = inventory_.grant_cargo(it, item_catalog());
    if (!o.ok) {
        spdlog::info("[session] {}: a quest gives the crate {}: it cannot be carried ({}), so it goes into the bag", name(), rec->name, o.refusal);
        return false;
    }
    apply_outcome(conn, o, "a quest crate was given");
    spdlog::info("[session] {}: a quest gives the crate {}: it is carried on the back", name(), rec->name);
    return true;
}

void PlayerSession::tick_quests(Connection& conn, Clock::time_point now) {
    // what the construction crates noted (a crate picked up at a station, crates put into a building under construction)
    while (!quest_crate_events_.empty()) {
        const CrateEvent e = quest_crate_events_.front();
        quest_crate_events_.pop_front();
        if (quest_log_.instances().empty()) continue;
        const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
        if (!qb) continue;
        int64_t taken = 0;
        if (!e.deposit) {
            quest_engine(conn, now, "a construction crate", [&](quest::Engine& eng) {
                eng.notified("ConstructionCrateEquipped", 0);
                eng.collected("Equip", e.item, 1);
            });
        } else {
            quest_engine(conn, now, "a construction deposit", [&](quest::Engine& eng) {
                for (int32_t i = 0; i < e.count; ++i) eng.notified("ContributedToServiceBuildingConstruction", 0);
                taken = eng.deposit(e.item, qb->crate_of(e.item), e.count);
            });
            spdlog::log(level_for(quests_logged_, 200), "[session] {}: {} crate(s) of item {} put into project {} of node {}{}", name(), e.count, e.item, e.project, e.node,
                        taken > 0 ? "; " + std::to_string(taken) + " counted for a crate objective" : std::string());
        }
    }
    // a quest that is done leaves the log a moment after the client was told it is done (the game takes it out a tick later; here the client has had 1.5 s to show it)
    if (quests_on_ && !quest_log_.instances().empty()) {
        constexpr double kDoneStays = 1.5;
        const double clock = quest_clock(now);
        std::vector<std::pair<int64_t, const char*>> leaving;
        for (const quest::QuestInstance& q : quest_log_.instances()) {
            if (q.status == quest::Status::Active || q.wire_dirty || clock - q.end_time < kDoneStays) continue;
            leaving.emplace_back(q.record, q.status == quest::Status::Completed ? "finish" : "fail");
        }
        for (const auto& [record, kind] : leaving) end_quest(conn, record, kind, now);
    }
    // rewards that waited for room in the reply queue
    while (!quest_rewards_waiting_.empty()) {
        const WaitingReward& w = quest_rewards_waiting_.front();
        if (!deliver_quest_rewards(conn, w.tables, w.why.c_str(), now, w.quest)) break;
        quest_rewards_waiting_.pop_front();
    }
    if (now - quest_checked_ < kQuestTick) return;
    quest_checked_ = now;
    tick_quest_givers(conn, now);   // (also for a character with an empty log: the NPCs around it have quests to give)
    if (quest_log_.instances().empty()) return;
    quest_engine(conn, now, "a quest tick", [&](quest::Engine& eng) {
        eng.settle();   // the items carried (Collect), the rules (Custom), the objectives that wake up
        if (phase_in_done_ && eng.wants_position()) {
            const GoodPosition at = checker_.good();
            eng.position(at.x, at.y, at.z);
        }
    });
}

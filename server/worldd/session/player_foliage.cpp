// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_foliage.cpp  (Gathering stage 3, part "Foliage gathering")
//
//  The player's side of cutting a painted tree or plant: the checks of the client's ServerStartInteractionWithFoliage, the channel's conditions, and the frame of the result.  The
//  world's side is foliage/foliage.h (the map, every instance's state and the cell actors that show it), the data and the rolls gather/gathering.h's, the requirements
//  gather/rules.h's, the items the common part of a harvest's (player_gather.cpp, deliver_harvest).  The order is the retail captures' [R: 1463 packets 308 to 836, 1464]:
//
//    the start     the client's call; 116 ms later the pawn's UPlayerInfo.FoliageData (the length, the start on the effects' clock, the instance, its names, the state, the tool: the client
//                  plays the animation and shows the bar from it); nothing else is sent while the channel runs
//    the result    ClientUpdateItemDurability, the storage events and ClientConfirmMultiInteractFoliage in one packet, the XPTracker, the cell's state actor with the instance Empty,
//                  FoliageData cleared 2 ms after the result
//    a refusal     for a start while the player moves: the callout "Economy / GatheringInterruption_Movement" and ClientInterruptInteractionWithFoliage(instance, owner, component), 105 ms
//                  after the start (1464).  Every other refusal (and a start worldd cannot read, or a world without foliage) sends the same call without the callout: the client sets its
//                  own FoliageData with the start and the call is what clears it [exe: ClientInterruptInteractionWithFoliage_Implementation; no capture shows another refusal]; only a
//                  second start while this player's own channel runs is not answered (the client's FoliageData is the running channel's)
//    a cancel      no reward, the instance free again, FoliageData cleared; worldd's own reasons (movement, death, mounting) tell the client as the movement refusal does, and one before
//                  FoliageData was sent gets the call without the callout
//
//  Every refusal and every cancellation is logged and counted; the first few of each kind at info.  The log lines the user's test reads start with "[foliage]".
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/constants.h"
#include "foliage/foliage.h"
#include "gather/rules.h"
#include "protocol/repl/fname.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// [R: capture 1463: the start at 4311 ms, FoliageData set at 4427 ms; the same 116 ms in 1428, 1463 to 1468] how long after the client's start the channel begins.
constexpr std::chrono::milliseconds kFoliageDataDelay{116};
/// [modelled from capture 1464's first start, refused: the client's last moving move was 44 ms before it, the second start 695 ms after the last] a start is refused when the player walked
/// (held a direction or moved) within this long.
constexpr std::chrono::milliseconds kMovingWindow{250};
/// As the nodes' channel: a fidget is not a walk; the player may stray this far (cm, horizontally) from where the channel began, and this far beyond the reach.
constexpr double kChannelDriftCm = 60.0;
constexpr double kChannelSlackCm = 100.0;
/// The callout the retail server shows for an interruption [R: 1464 packet 191: type 4, point size 32, the string table "Economy"].
constexpr int32_t kInterruptCalloutType = 4, kCalloutPointSize = 32;
constexpr uint64_t kInterruptChatCategory = 4;   // [R: 1464 packet 191: ChatCategory 4]
constexpr std::chrono::seconds kCalloutLife{3};   // [R: 1464 packet 397: the callout is taken off 3 s later]

}  // namespace

const char* PlayerSession::interrupt_key(Interrupt k) {
    switch (k) {
        case Interrupt::Movement: return "GatheringInterruption_Movement";   // [R: 1464]
        case Interrupt::Death: return "GatheringInterruption_Death";         // [exe: 0xb92a218 .. 0xb92a298; modelled]
        case Interrupt::Mounted: return "GatheringInterruption_Mounted";
        case Interrupt::Generic: return "GatheringInterruption_Generic";
        case Interrupt::None: break;
    }
    return "";
}

void PlayerSession::send_foliage_data(Connection& conn, const bootstrap::PlayerBootstrap::FoliageChannel& data) {
    queue("the pawn's foliage data", [data](bootstrap::PlayerBootstrap& pb) { return pb.pawn_foliage_data(data); }, conn, /*must_build=*/true);
}
// == C-O-A | Cyb ==

void PlayerSession::send_interrupt_callout(Connection& conn, Interrupt key, Clock::time_point now) {
    if (key == Interrupt::None || !room_for(2)) return;
    const int64_t unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t ticks = items::ticks_of_ms(unix_ms);
    const uint64_t callout_key = (static_cast<uint64_t>(pb_.setup().player_id) << 32) | (0x40000000u + gather_callout_seq_++);
    const std::string text = interrupt_key(key);
    queue("a callout", [ticks, callout_key, text](bootstrap::PlayerBootstrap& pb) { return pb.callout_add("", kInterruptCalloutType, kCalloutPointSize, ticks, callout_key, "Economy", text, kInterruptChatCategory); }, conn);
    gather_callouts_.push_back({now + kCalloutLife, callout_key});
}

void PlayerSession::unlock_foliage_client(Connection& conn, uint32_t index, const std::string& owner, const std::string& component) {
    // The client sets its own pawn's FoliageData the moment it sends ServerStartInteractionWithFoliage and answers every other gatherable with "Unable to interact with Item while
    // gathering!" (FoliageData.OriginalInstanceIndex >= 0) until the server clears it.  A cutting worldd refuses or ends without ever having told the client FoliageData leaves
    // nothing to clear in worldd's own shadow of the pawn (a clear block would be an empty diff), so the call is what gives the client its gathering back: the exe's
    // AAoCPlayerController::ClientInterruptInteractionWithFoliage_Implementation sets the pawn's FoliageData to its cleared value (index -1, state 3) and stops the
    // animation, and reads none of its parameters.  Retail sends it with the callout for a start while walking [R: 1464 packet 191]; the trees' retests 3 and 4 stalled for want of it.
    if (!room_for(2)) return;
    queue("ClientInterruptInteractionWithFoliage", [index, owner, component](bootstrap::PlayerBootstrap& pb) { return pb.foliage_interrupt(index, owner, component); }, conn);
    ++foliage_interrupts_sent_;
}

void PlayerSession::on_foliage_interact(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    ++foliage_interacts_;
    foliage::Foliage* f = npc_world_ ? npc_world_->foliage() : nullptr;
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    // the call's parameters when they decoded: the instance (left out when 0), its owner and component by name, the state the client sees it in
    const bool decoded = rpc.call.rpc && rpc.call.decoded;
    const auto value = [&](const char* path) -> const repl::Value* {
        const auto* cmd = decoded ? repl::rpc_command(*rpc.call.rpc, path) : nullptr;
        if (!cmd) return nullptr;
        const auto it = rpc.call.params.values.find(cmd->handle);
        return it == rpc.call.params.values.end() ? nullptr : &it->second;
    };
    const repl::Value* iv = value("OriginalInstanceIndex");
    const repl::Value* ov = value("FoliageName");
    const repl::Value* cv = value("FoliageComponentUUID");
    const repl::Value* sv = value("FoliageState");
    const uint32_t index = iv && iv->get_if<int64_t>() ? static_cast<uint32_t>(*iv->get_if<int64_t>()) : 0u;
    const std::string owner = ov && ov->get_if<repl::NameValue>() ? repl::fname_text(*ov->get_if<repl::NameValue>()) : std::string();
    const std::string component = cv && cv->get_if<repl::NameValue>() ? repl::fname_text(*cv->get_if<repl::NameValue>()) : std::string();
    const int state = sv && sv->get_if<uint64_t>() ? static_cast<int>(*sv->get_if<uint64_t>()) : -1;
    const char* state_name = state >= foliage::kInfancy && state <= foliage::kEmpty ? foliage::kStateNames[state] : "?";
    const std::string what = owner + " / " + component + " #" + std::to_string(index) + " (" + state_name + ")";
    if (!f || !g) {
        if (!foliage_off_logged_) {
            foliage_off_logged_ = true;
            spdlog::info("[gather] {}: a tree or plant of the world's foliage was interacted with (ServerStartInteractionWithFoliage), and foliage gathering is off in this world (--no-gathering, "
                         "or content.db has no foliage tables, or dist/content/region_foliage.tsv did not load: see the log of the start); further foliage interactions are not logged",
                         name());
        }
        ++foliage_refused_;
        ++foliage_refusals_["foliage off"];
        unlock_foliage_client(conn, index, owner, component);
        return;
    }
    if (!decoded) {
        ++foliage_refused_;
        ++foliage_refusals_["unreadable"];
        unlock_foliage_client(conn, 0, std::string(), std::string());   // (the call's names did not decode: the client's own are not needed, the interrupt reads none)
        return;
    }
    const auto refuse = [&](const char* kind, const std::string& why, Interrupt interrupt = Interrupt::None) {
        ++foliage_refused_;
        ++foliage_refusals_[kind];
        spdlog::log(level_for(foliage_refused_logged_, 12), "[foliage] {}: the cutting of {} is refused ({}): {}", name(), what, kind, why);
        if (interrupt != Interrupt::None && room_for(4)) send_interrupt_callout(conn, interrupt, now);   // [R: 1464 packet 191] the callout and the interrupt, in one packet
        // Every refusal gives the client its gathering back (unlock_foliage_client: it set its own FoliageData with the start): retail says so for a start while walking
        // (1464) and no capture shows another refusal, so the rest are the same call without a callout.  Except a second start while this player's own TREE channel runs: the client's
        // FoliageData then is the running channel's, and the interrupt would end it on the client alone.  A NODE's or a hunt's channel (gather_.node) is not FoliageData's: the client sets
        // its own FoliageData with this start all the same (nothing on its way from the key to the RPC reads ChannelingData, a montage or an interact state: the exe's
        // AAoCPlayerController::CanInteractWithFoliage 0x146a92000 asks for FoliageData, standing still, alive, on foot, not crowd-controlled, the requirements and the geometry), so the
        // refusal unlocks it, or every later tree answers "Unable to interact with Item while gathering!" until the relog (the pre-merge review of 2026-10-16, A1).
        if (foliage_.row == kNoInstance) unlock_foliage_client(conn, index, owner, component);
    };
    if (dead_) return refuse("dead", "the player is dead");
    if (mounted()) return refuse("mounted", "the player is on a mount");
    if (foliage_.row != kNoInstance || gather_.node != 0) return refuse("already channeling", "this player is harvesting something already");
    const auto found = f->map().find(owner, component, index);
    if (!found) return refuse("unknown instance", "the world has no such instance (not in dist/content/region_foliage.tsv: a town's runtime tree, or a name the map does not know)");
    const uint32_t row = *found;
    const foliage::DefInfo& def = f->def_of(row);
    const int now_state = f->store().state_of(row);
    if (state != now_state) {
        return refuse("wrong state", std::string("the client sees it ") + state_name + ", the world has it " + (now_state >= 0 && now_state <= 3 ? foliage::kStateNames[now_state] : "?"));
    }
    const content::GatherDefRow* interaction = state >= foliage::kInfancy && state <= foliage::kAdult ? def.interactions[state] : nullptr;
    if (!interaction) return refuse("no interaction", def.name + " has no harvest in that state");
    const GoodPosition at = checker_.good();
    // not while walking [modelled from 1464's first start, which the retail server refused with the callout and the interrupt]
    if (moved_at_ != Clock::time_point{} && now - moved_at_ < kMovingWindow) {
        return refuse("moving", "the player moved " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - moved_at_).count()) + " ms ago (a cutting needs standing still: at least " +
                                    std::to_string(kMovingWindow.count()) + " ms)",
                      Interrupt::Movement);
    }
    const foliage::FoliageMap::Row& r = f->map().rows()[row];
    const double distance = std::hypot(static_cast<double>(r.x) - at.x, static_cast<double>(r.y) - at.y);
    const double reach = f->reach_cm(row, state);
    if (distance > reach) return refuse("out of range", std::to_string(static_cast<int>(distance)) + " cm away, the interaction reaches " + std::to_string(static_cast<int>(reach)) + " cm");
    if (const uint64_t holder = f->store().locked_by(row); holder != 0 && holder != static_cast<uint64_t>(pb_.setup().player_id)) {
        return refuse("instance busy", "another player is cutting it");
    }
    // the requirements: certification, level, the tool, combat
    gather::ToolInfo tool;
    const svc::InventoryItem* worn = inventory_.at(svc::ItemContainer::Equipment, interaction->tool_slot);
    if (worn) {
        tool.worn = true;
        tool.tool = g->tool(worn->item_record_id);
        tool.durability = worn->durability;
    }
    const bool in_combat = stats_ && stats_->in_combat();
    const gather::Verdict verdict = gather::check_requirements(*interaction, professions_, tool, in_combat);
    if (!verdict.ok()) return refuse(gather::refusal_name(verdict.why), verdict.text);
    std::string why;
    auto run = gather_run(why);
    if (!run) return refuse("no stats", why);
    const auto seconds = g->channel_seconds(*interaction, *run, why);
    if (!seconds) return refuse("no duration", why);
    const uint64_t me = static_cast<uint64_t>(pb_.setup().player_id);
    if (!f->store().lock(row, me)) return refuse("instance busy", "another player is cutting it");
// ~ C Y B :: CoA ~

    foliage_ = FoliageRun{};
    foliage_.row = row;
    foliage_.state = state;
    foliage_.index = index;
    foliage_.owner = owner;
    foliage_.component = component;
    foliage_.interaction = interaction;
    foliage_.name = def.name;
    foliage_.tool_slot = interaction->tool_slot;
    foliage_.tool_guid = worn->instance_guid;
    foliage_.tool_record = worn->item_record_id;
    foliage_.started = now;
    foliage_.data_at = now + kFoliageDataDelay;
    foliage_.seconds = *seconds;
    foliage_.completes = foliage_.data_at + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(*seconds));
    foliage_.start_x = at.x;
    foliage_.start_y = at.y;
    ++foliage_started_;
    const professions::Profession* profession = professions_.valid() ? professions_.book()->find(interaction->profession_guid) : nullptr;
    spdlog::log(level_for(foliage_logged_, 20), "[foliage] {}: starts cutting {} ({}): a {} {} {} with {}, a channel of {:.2f} s (base {:.2f} s), {:.0f} cm from it; the instance is whole again {:.0f} s after the harvest",
                name(), def.name, what, state_name, professions_.valid() ? professions_.book()->tier(interaction->cert_tier).name : std::string("?"), profession ? profession->name : std::string("?"),
                tool.tool ? tool.tool->item_name : std::string("?"), *seconds, interaction->base_duration, distance, f->store().regrow_seconds(row));
}

void PlayerSession::run_foliage(Connection& conn, Clock::time_point now) {
    foliage::Foliage* f = npc_world_ ? npc_world_->foliage() : nullptr;
    if (f) f->tick(now);   // any session's tick brings the regrown trees back
    if (foliage_.row == kNoInstance) return;
    if (!f) return cancel_foliage(&conn, "foliage gathering went away");
    // the channel's conditions: the player alive and on foot, the same tool in the slot, still, in reach
    if (dead_) return cancel_foliage(&conn, "the player is dead", Interrupt::Death);
    if (mounted()) return cancel_foliage(&conn, "the player is on a mount", Interrupt::Mounted);
    const svc::InventoryItem* tool = inventory_.at(svc::ItemContainer::Equipment, foliage_.tool_slot);
    if (!tool || tool->instance_guid != foliage_.tool_guid) return cancel_foliage(&conn, tool ? "the tool was swapped for another" : "the tool was taken off");
    if (!(tool->durability > 0)) return cancel_foliage(&conn, "the tool broke");
    const GoodPosition at = checker_.good();
    const foliage::FoliageMap::Row& r = f->map().rows()[foliage_.row];
    const double distance = std::hypot(static_cast<double>(r.x) - at.x, static_cast<double>(r.y) - at.y);
    const double reach = f->reach_cm(foliage_.row, foliage_.state);
    if (distance > reach + kChannelSlackCm) {
        return cancel_foliage(&conn, "the player moved away (" + std::to_string(static_cast<int>(distance)) + " cm from it, the interaction reaches " + std::to_string(static_cast<int>(reach)) + " cm)",
                              Interrupt::Movement);
    }
    const double drift = std::hypot(at.x - foliage_.start_x, at.y - foliage_.start_y);
    if (drift > kChannelDriftCm) {
        return cancel_foliage(&conn, "the player moved " + std::to_string(static_cast<int>(std::lround(drift))) + " cm since the channel began (a cutting needs standing still; at most " +
                                         std::to_string(static_cast<int>(kChannelDriftCm)) + " cm)",
                              Interrupt::Movement);
    }
    // 116 ms after the start the channel begins on the client: FoliageData
    if (!foliage_.data_sent && now >= foliage_.data_at) {
        if (!room_for(2)) return;
        bootstrap::PlayerBootstrap::FoliageChannel d;
        d.duration = static_cast<float>(foliage_.seconds);
        d.start_time = bootstrap::effect_clock(pb_.setup().world_time + session_seconds(foliage_.data_at));   // the effects' clock, not whole seconds [R: 1463 13900366.812]
        d.instance = foliage_.index;
        d.owner = foliage_.owner;
        d.component = foliage_.component;
        d.interaction_index = 0;
        d.state = static_cast<uint8_t>(foliage_.state);
        d.tool_record = foliage_.tool_record;
        if (const auto guid = items::guid_of(foliage_.tool_guid)) d.tool_guid = {static_cast<uint32_t>(guid->a), static_cast<uint32_t>(guid->b), static_cast<uint32_t>(guid->c), static_cast<uint32_t>(guid->d)};
        send_foliage_data(conn, d);
        foliage_.data_sent = true;
    }
    if (foliage_.data_sent && now >= foliage_.completes) complete_foliage(conn, now);
}

bool PlayerSession::complete_foliage(Connection& conn, Clock::time_point now) {
    foliage::Foliage* f = npc_world_ ? npc_world_->foliage() : nullptr;
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!f || !g || !db || !foliage_.interaction) {
        cancel_foliage(&conn, "foliage gathering went away");
        return true;
    }
    const size_t grants = world_ ? world_->auto_grants().size() : 0;
    if (!room_for(16 + grants)) return false;   // the frame waits for room in the reply queue; the channel's clock is over
    const uint32_t row = foliage_.row;
    std::string why;
    auto run = gather_run(why);
    if (!run) {
        cancel_foliage(&conn, "no stat run for the rolls: " + why);
        return true;
    }
    const foliage::FoliageMap::Row& r = f->map().rows()[row];
    const world::CityNodes* nodes = world_->city_nodes();
    const world::NodeSite site = nodes ? nodes->node_at(r.x, r.y) : world::NodeSite{};
    const auto harvest = g->harvest(*foliage_.interaction, *run, progress_.level, site, nodes, g->rng(), why);
    if (!harvest) {
        cancel_foliage(&conn, "the rolls do not run: " + why);
        return true;
    }
    HarvestFrame frame;
    frame.tag = "foliage";
    frame.what = foliage_.name + " (" + foliage_.owner + " / " + foliage_.component + " #" + std::to_string(foliage_.index) + ")";
    frame.profession = foliage_.interaction->profession_guid;
    frame.harvest = &*harvest;
    frame.tool_guid = foliage_.tool_guid;
    frame.tool_slot = foliage_.tool_slot;
    // [R: 1463 packet 833] the confirm follows the storage events in the same packet; [packets 835 and 836] the state actor's change, then FoliageData cleared
    frame.after_items = [&conn, this] { queue("ClientConfirmMultiInteractFoliage", [](bootstrap::PlayerBootstrap& pb) { return pb.foliage_confirm(); }, conn, /*must_build=*/true); };
    frame.after_experience = [&conn, f, row, now, this] {
        f->harvested(row, now);
        send_foliage_data(conn, bootstrap::PlayerBootstrap::FoliageChannel{});
    };
    char tail[96];
    std::snprintf(tail, sizeof tail, "; the instance is whole again in %.0f s", f->store().regrow_seconds(row));
    frame.log_tail = tail;
    f->store().unlock(row, static_cast<uint64_t>(pb_.setup().player_id));
    foliage_ = FoliageRun{};
    ++foliage_completed_;
    deliver_harvest(conn, now, frame);
    return true;
}
// [C O A x C | Y | B]

void PlayerSession::cancel_foliage(Connection* conn, const std::string& why, Interrupt interrupt) {
    if (foliage_.row == kNoInstance) return;
    foliage::Foliage* f = npc_world_ ? npc_world_->foliage() : nullptr;
    const FoliageRun run = foliage_;
    foliage_ = FoliageRun{};
    if (f) f->store().unlock(run.row, static_cast<uint64_t>(pb_.setup().player_id));
    ++foliage_cancelled_;
    spdlog::log(level_for(foliage_logged_, 20), "[foliage] {}: the cutting of {} ({} / {} #{}) ends with no reward: {}", name(), run.name, run.owner, run.component, run.index, why);
    if (!conn) return;
    if (run.data_sent) send_foliage_data(*conn, bootstrap::PlayerBootstrap::FoliageChannel{});   // the client's animation and bar end with the channel [inferred: the result's clear]
    if (interrupt != Interrupt::None && room_for(4)) send_interrupt_callout(*conn, interrupt, last_now_);
    // The interrupt goes with every cancellation that says why, and with one before FoliageData was sent (the first 116 ms, or a start worldd ended at once): the client set its
    // own FoliageData with the start, a clear of ours would be an empty diff, and nothing else would give it its gathering back (unlock_foliage_client).
    if (interrupt != Interrupt::None || !run.data_sent) unlock_foliage_client(*conn, run.index, run.owner, run.component);
}

}}  // namespace aoc::worldd

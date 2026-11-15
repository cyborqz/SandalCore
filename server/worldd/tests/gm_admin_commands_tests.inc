// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/gm_admin_commands_tests.inc  (included by test_worldd_gm_travel.cpp)
//
//  The GM command list and the run call (WorldAdminService ListCommands and RunCommand, session/admin_commands.h and session/gm_travel.cpp), and the game-master test commands it
//  dispatches: the list is complete (every WorldAdmin method, the travel tools and the NPC actions included), every listed command is dispatched, every field is held to the list's
//  own rules, a character that is not in the world is answered "not in the world", each command's effect is on a joined session with a fake client, and one log line is written per
//  run.  The quest commands use the real content.db's quests around a real quest worker, as the quest session tests do.
// ============================================================================
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "quest/engine.h"
#include "quest/quest_book.h"
#include "quest/quest_service.h"
#include "session/admin_commands.h"
#include "svc/services.h"

#include <mutex>

namespace gma = aoc::worldd::admin;
namespace qs = aoc::worldd::quest;

static constexpr int64_t kDodgeAbility = 107258680465016ll;   // General_Dodge: a 0.65 s cooldown the test's ability table gives it

// ---- the admin API over the harness's one player ----------------------------------------------------------------------------------------------------------------

struct AdminRun {
    grpc::Status                  status;
    adminv1::RunCommandResponse   response;
};

static AdminRun admin_run(Admin& a, const std::string& command, const std::map<std::string, std::string>& fields) {
    grpc::ClientContext ctx;
    Admin::authorize(ctx, kToken);
    adminv1::RunCommandRequest rq;
    rq.set_command(command);
    for (const auto& [name, value] : fields) (*rq.mutable_fields())[name] = value;
    adminv1::RunCommandResponse rs;
    const grpc::Status s = a.via_net_thread([&] { return a.stub->RunCommand(&ctx, rq, &rs); });
    return {s, std::move(rs)};
}

static std::vector<adminv1::AdminCommand> admin_command_list(Admin& a) {
    grpc::ClientContext ctx;
    Admin::authorize(ctx, kToken);
    adminv1::ListCommandsResponse rs;
    const grpc::Status s = a.via_net_thread([&] { return a.stub->ListCommands(&ctx, adminv1::ListCommandsRequest(), &rs); });
    if (!s.ok()) return {};
    return {rs.commands().begin(), rs.commands().end()};
}

static const adminv1::AdminCommand* admin_command_named(const std::vector<adminv1::AdminCommand>& commands, const std::string& name) {
    for (const adminv1::AdminCommand& c : commands) {
        if (c.name() == name) return &c;
    }
    return nullptr;
}

static const adminv1::CommandField* admin_field_named(const adminv1::AdminCommand& command, const std::string& name) {
    for (const adminv1::CommandField& f : command.fields()) {
        if (f.name() == name) return &f;
    }
    return nullptr;
}

static bool one_line(const std::string& s) { return !s.empty() && s.find('\n') == std::string::npos && s.find('\r') == std::string::npos; }

static size_t calls_named(const Harness& h, uint32_t channel, size_t from, const char* cls, const char* name) {
    size_t n = 0;
    for (const fake::Delivered* d : chains_on(h, channel, from)) {
        for (const repl::RpcCall& c : calls_of(*d, cls)) n += c.rpc && std::string(c.rpc->name) == name;
    }
    return n;
}

/// The game time of the last ClientSetTime the client received from `from` on (the caller has stepped).
static std::optional<double> last_clock_game_time(const Harness& h, size_t from) {
    for (size_t i = from; i < h.got.size(); ++i) {
        for (const repl::RpcCall& c : calls_of(h.got[i], "AoCPlayerControllerBP_C")) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientSetTime") continue;
            const repl::Value* v = repl::rpcf::value_of(*c.rpc, c.params, "NewGameTime");
            if (v) {
                if (const auto* d = v->get_if<double>()) return *d;
            }
        }
    }
    return std::nullopt;
}

static double stat_of(const PlayerSession& s, int64_t record) {
    const stats::CharacterStats* st = s.character_stats();
    const stats::StatValue* v = st ? st->find(record) : nullptr;
    return v ? static_cast<double>(v->value) : -1.0;
}

// ---- a real quest to command ------------------------------------------------------------------------------------------------------------------------------------

/// Two quests of the real content.db with an awake first objective (a GM start activates it), the quests and dialogue the world needs.
struct QuestPick {
    std::optional<content::QuestData> quest_data;
    std::optional<dialogue::Book>     dialogue;
    struct One {
        int64_t     quest = 0;
        int64_t     objective = 0;
        std::string name;
    };
    One first, second;

    bool load(std::string* why) {
        auto db = content::ContentDb::open(real_db_path(), *why);
        if (!db) return false;
        auto d = db->dialogue_data(*why);
        if (!d) return false;
        auto q = db->quest_data(*why);
        if (!q) return false;
        for (const auto& [guid, row] : q->quests) {
            if (row.disabled || !row.default_activate || row.objectives.empty()) continue;
            if (!row.objectives.front().awake) continue;
            One* slot = first.quest == 0 ? &first : (second.quest == 0 ? &second : nullptr);
            if (!slot) break;
            slot->quest = guid;
            slot->objective = row.objectives.front().guid;
            slot->name = row.name;
        }
        if (first.quest == 0 || second.quest == 0) {
            *why = "the content.db has fewer than two quests that start with an awake objective";
            return false;
        }
        quest_data = std::move(*q);
        dialogue.emplace(std::move(*d));
        return true;
    }
};

/// The quest service in memory (the session's worker polls it; a start's save is read back by a relog, which these tests do not do).
struct QuestStub : svc::QuestApi {
    svc::QuestLogResult load_log(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mu);
        svc::QuestLogResult r;
        r.status = svc::CallStatus::Ok;
        r.log = logs[id];
        return r;
    }
    svc::CallStatus save_log(const std::string& id, const svc::SavedQuestLog& log) override {
        std::lock_guard<std::mutex> lock(mu);
        logs[id] = log;
        ++saves;
        return svc::CallStatus::Ok;
    }
    svc::CallStatus take_commands(const std::string&, std::vector<svc::QuestCommand>&) override { return svc::CallStatus::Ok; }

    std::mutex mu;
    std::map<std::string, svc::SavedQuestLog> logs;
    size_t saves = 0;
};

// ---- the list ---------------------------------------------------------------------------------------------------------------------------------------------------

static void test_gm_admin_command_list_is_complete() {
    std::printf("the GM command list: every command the server offers, with its fields\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    Admin a(h);
    const std::vector<adminv1::AdminCommand> commands = admin_command_list(a);
    CHECK(!commands.empty(), "ListCommands answers with the command list");
    if (commands.empty()) return;
    CHECK(commands.size() >= 34, "the list holds the travel tools, the NPC actions and the game-master test commands (34 at least)");
    std::set<std::string> names;
    for (const adminv1::AdminCommand& c : commands) CHECK(names.insert(c.name()).second, (std::string("the name ") + c.name() + " is in the list once").c_str());
    // every WorldAdminService method the server had before this change is still a command
    for (const char* name : {"list-players", "list-places", "teleport", "speed", "give-money", "give-item", "place-station", "remove-station", "finish-jobs", "teach-recipe",
                             "forget-recipe", "list-taught-recipes", "reset-tutorials", "weapon-xp", "npc-kill", "npc-respawn", "npc-reset", "npc-heal", "npc-despawn", "npc-move"}) {
        CHECK(admin_command_named(commands, name) != nullptr, (std::string("the existing command ") + name + " is in the list").c_str());
    }
    // and the game-master test commands of this change
    for (const char* name : {"heal", "set-health", "set-mana", "revive", "kill", "give-experience", "start-quest", "complete-quest", "complete-objective", "reset-quests",
                             "clear-cooldowns", "give-mount", "set-time", "node-xp"}) {
        CHECK(admin_command_named(commands, name) != nullptr, (std::string("the test command ") + name + " is in the list").c_str());
    }
    bool descriptions = true, groups = true, labels = true, rules = true, character_fields = true;
    for (const adminv1::AdminCommand& c : commands) {
        descriptions = descriptions && !c.description().empty() && c.description().back() == '.';
        groups = groups && !c.group().empty();
        std::set<std::string> field_names;
        for (const adminv1::CommandField& f : c.fields()) {
            labels = labels && !f.name().empty() && !f.label().empty() && !f.hint().empty() && f.type() != adminv1::COMMAND_FIELD_TYPE_UNSPECIFIED && field_names.insert(f.name()).second;
            rules = rules && (!f.has_minimum() || !f.has_maximum() || f.minimum() <= f.maximum()) && (f.type() != adminv1::COMMAND_FIELD_TYPE_CHOICE || f.choices_size() > 0);
        }
        if (c.acts_on_character()) {
            const adminv1::CommandField* f = admin_field_named(c, "character");
            character_fields = character_fields && f && f->type() == adminv1::COMMAND_FIELD_TYPE_CHARACTER && f->required();
        }
    }
    CHECK(descriptions, "every command has a one-sentence description");
    CHECK(groups, "every command has a group");
    CHECK(labels, "every field has a name, a label, a hint, a type, and no field name is used twice in a command");
    CHECK(rules, "every range is ordered and every choice has values");
    CHECK(character_fields, "every command that acts on a character has a required character field");
    const adminv1::AdminCommand* set_health = admin_command_named(commands, "set-health");
    const adminv1::CommandField* character = set_health ? admin_field_named(*set_health, "character") : nullptr;
    const adminv1::CommandField* value = set_health ? admin_field_named(*set_health, "value") : nullptr;
    CHECK(set_health && set_health->acts_on_character() && character && character->type() == adminv1::COMMAND_FIELD_TYPE_CHARACTER && value &&
              value->type() == adminv1::COMMAND_FIELD_TYPE_NUMBER && value->required() && value->has_minimum() && value->minimum() == 0,
          "set-health's example entry: a required character and a number value from 0");
    const adminv1::AdminCommand* teleport = admin_command_named(commands, "teleport");
    CHECK(teleport && admin_field_named(*teleport, "place") && admin_field_named(*teleport, "x") && admin_field_named(*teleport, "y") && admin_field_named(*teleport, "z") &&
              !admin_field_named(*teleport, "place")->required(),
          "teleport's entry: a place and a point, each optional (one of them is needed at run time)");
}
AOC_REGISTER_TEST(test_gm_admin_command_list_is_complete);

// ---- the run call's field rules ----------------------------------------------------------------------------------------------------------------------------------

static void test_gm_admin_run_checks_every_field() {
    std::printf("the run call: every field is checked against the list's rules before anything runs\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const auto run = [&](const char* command, std::map<std::string, std::string> fields) { return travel.run_command(command, fields, "the test"); };
    CHECK(run("no-such-command", {}).status == GmStatus::InvalidArgument, "an unknown command is refused");
    CHECK(run("heal", {}).message.find("required") != std::string::npos, "a missing required field is refused, and the answer says so");
    CHECK(run("heal", {{"character", "cyb"}, {"colour", "red"}}).status == GmStatus::InvalidArgument,
          "a field the command does not have is refused (the caller's map is not trusted)");
    CHECK(run("speed", {{"character", "cyb"}, {"multiplier", "fast"}}).message.find("number") != std::string::npos, "a number that is not one is refused");
    CHECK(run("speed", {{"character", "cyb"}, {"multiplier", "11"}}).message.find("0.5 to 10") != std::string::npos, "a number outside the list's range is refused");
    CHECK(run("give-item", {{"character", "cyb"}, {"item", "anything"}, {"rarity", "Purple"}}).message.find("must be one of") != std::string::npos,
          "a choice that is not one of the list's values is refused");
    const GmRunResult nobody = run("heal", {{"character", "nobody"}});
    std::printf("  [info] heal on nobody: status %d, \"%s\"\n", static_cast<int>(nobody.status), nobody.message.c_str());
    CHECK(nobody.status == GmStatus::NotFound && nobody.message.find("not in the world") != std::string::npos, "a character that is not in the world is answered \"not in the world\"");
    // a command whose fields are all right is accepted, and a field a form leaves empty counts as no value
    CHECK(run("speed", {{"character", "cyb"}, {"multiplier", "2"}}).status == GmStatus::Ok, "a correct field set is accepted");
    CHECK(run("give-item", {{"character", "cyb"}, {"item", "anything"}, {"rarity", ""}}).message.find("must be one of") == std::string::npos,
          "an empty value is no value, not a choice to check");
    // every command of the list refuses a field it does not have, and accepts its own fields as its types say (the list is the only source of names)
    for (const gma::AdminCommand& c : gma::admin_commands()) {
        if (c.fields.empty()) continue;
        const GmRunResult wrong = run(c.name.c_str(), {{"not-a-field", "1"}});
        CHECK(wrong.status == GmStatus::InvalidArgument && wrong.message.find("has no field") != std::string::npos,
              (std::string("the command ") + c.name + " refuses a field it does not have").c_str());
    }
}
AOC_REGISTER_TEST(test_gm_admin_run_checks_every_field);

static void test_gm_admin_every_listed_command_is_dispatched() {
    std::printf("the run call: every command in the list reaches its handler\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    bool all = true;
    for (const gma::AdminCommand& c : gma::admin_commands()) {
        const GmRunResult r = travel.run_command(c.name, {}, "the test");
        // with no fields the command is refused for its required fields (or it runs: list-players, list-places); never as an unknown command
        const bool known = r.message.find("no command is named") == std::string::npos && r.message.find("no command is named \"" + c.name) == std::string::npos;
        if (!known) std::printf("  [info] %s: %s\n", c.name.c_str(), r.message.c_str());
        all = all && known;
    }
    CHECK(all, "every command the list shows is one the run call knows (no list entry without a handler)");
    CHECK(travel.run_command("heal", {{"character", "cyb"}}, "the test").status == GmStatus::Ok, "and a command from the list runs (heal)");
    CHECK(gma::find_command("HeAl") != nullptr && gma::find_command("heal") == gma::find_command("HeAl"), "a command's name is matched without case");
    CHECK(gma::find_command("no-such-command") == nullptr, "a name that is not in the list is not found");
}
AOC_REGISTER_TEST(test_gm_admin_every_listed_command_is_dispatched);

static void test_gm_admin_run_over_the_admin_api() {
    std::printf("the admin API: RunCommand behind the token, applied on the network thread\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    Admin a(h);
    const GoodPosition start = h.s->movement().good();
    {
        grpc::ClientContext ctx;   // no token
        adminv1::RunCommandRequest rq;
        rq.set_command("list-players");
        adminv1::RunCommandResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->RunCommand(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a run with no token is refused (Unauthenticated)");
    }
    const AdminRun listed = admin_run(a, "list-players", {});
    CHECK(listed.status.ok() && listed.response.result().find("cyb") != std::string::npos && one_line(listed.response.result()), "RunCommand runs list-players and answers one line");
    CHECK(!a.directory_off_thread, "(the run was applied on the network thread)");
    const AdminRun moved = admin_run(a, "teleport", {{"character", "cyb"}, {"place", "joeva"}});
    const world::CityNodeInfo* joeva = g_nodes->by_name("Joeva");
    CHECK(moved.status.ok() && moved.response.character() == "cyb" && joeva && near_to(h.s->movement().good().x, joeva->x, 0.01),
          "RunCommand runs teleport to a place: the player moved");
    CHECK(moved.response.result().find("moved") != std::string::npos && one_line(moved.response.result()), "... and the answer is one plain line");
    const AdminRun bad = admin_run(a, "speed", {{"character", "cyb"}, {"multiplier", "99"}});
    CHECK(bad.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT, "a bad field is InvalidArgument over the API, the same check as the direct call");
    CHECK(near_to(h.s->movement().good().x, joeva->x, 0.01), "(the refused run changed nothing)");
    const AdminRun nobody = admin_run(a, "heal", {{"character", "nobody"}});
    CHECK(nobody.status.error_code() == grpc::StatusCode::NOT_FOUND && nobody.status.error_message().find("not in the world") != std::string::npos,
          "an unknown character is NotFound, saying it is not in the world");
    CHECK(!a.directory_off_thread, "(no run touched the sessions off the network thread)");
}
AOC_REGISTER_TEST(test_gm_admin_run_over_the_admin_api);

// ---- the character commands -------------------------------------------------------------------------------------------------------------------------------------

static void test_gm_admin_heal_and_set_values() {
    std::printf("heal, set-health and set-mana: the stats and the client's ability update\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const auto run = [&](const char* command, std::map<std::string, std::string> fields) { return travel.run_command(command, fields, "the test"); };
    const double max_health = stat_of(*h.s, content::kGuidMaxHealth), max_mana = stat_of(*h.s, content::kGuidMaxMana);
    CHECK(max_health > 0 && max_mana > 0, "(the character has Health and Mana)");
    const size_t from = h.got.size();
    CHECK(run("set-health", {{"character", "cyb"}, {"value", "10"}}).status == GmStatus::Ok, "set-health sets Health");
    CHECK(near_to(stat_of(*h.s, content::kGuidHealth), 10.0, 0.01), "the session's Health is 10");
    h.step();
    CHECK(!chains_on(h, h.pawn_ch, from).empty(), "the client is sent the ability update of the Health change");
    CHECK(run("set-mana", {{"character", "cyb"}, {"value", "5"}}).status == GmStatus::Ok, "set-mana sets Mana");
    CHECK(near_to(stat_of(*h.s, content::kGuidMana), 5.0, 0.01), "the session's Mana is 5");
    const GmRunResult heal = run("heal", {{"character", "cyb"}});
    CHECK(heal.status == GmStatus::Ok && near_to(stat_of(*h.s, content::kGuidHealth), max_health, 0.01) && near_to(stat_of(*h.s, content::kGuidMana), max_mana, 0.01),
          "heal puts Health and Mana back to their maximum");
    CHECK(heal.message.find("full Health and Mana") != std::string::npos, "... and says so in one line");
    CHECK(run("set-health", {{"character", "cyb"}, {"value", "9999999"}}).status == GmStatus::Ok && near_to(stat_of(*h.s, content::kGuidHealth), max_health, 0.01),
          "a Health above the maximum is held at the maximum");
    CHECK(run("set-health", {{"character", "cyb"}, {"value", "-1"}}).status == GmStatus::InvalidArgument, "a Health below the list's range is refused");
    CHECK(run("set-health", {{"character", "cyb"}, {"value", "many"}}).status == GmStatus::InvalidArgument, "a Health that is not a number is refused");
    // a dead character is refused by both, and the revive is the door
    CHECK(run("kill", {{"character", "cyb"}}).status == GmStatus::Ok && h.s->dead(), "(the character is dead for the next check)");
    CHECK(run("heal", {{"character", "cyb"}}).status == GmStatus::FailedPrecondition && run("heal", {{"character", "cyb"}}).message.find("dead") != std::string::npos,
          "a dead character is not healed: it is revived first");
    CHECK(run("revive", {{"character", "cyb"}}).status == GmStatus::Ok && !h.s->dead(), "(revived afterward)");
}
AOC_REGISTER_TEST(test_gm_admin_heal_and_set_values);

static void test_gm_admin_kill_and_revive() {
    std::printf("kill and revive: the death's frames and the respawn's correction\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const auto run = [&](const char* command, std::map<std::string, std::string> fields) { return travel.run_command(command, fields, "the test"); };
    const size_t from = h.got.size();
    const GmRunResult killed = run("kill", {{"character", "cyb"}});
    CHECK(killed.status == GmStatus::Ok && h.s->dead() && h.s->deaths() == 1, "kill kills the character (one death)");
    CHECK(killed.message.find("was killed") != std::string::npos, "... and says so");
    h.run(milliseconds(100));
    CHECK(calls_named(h, h.pc_ch, from, "AoCPlayerControllerBP_C", "ClientRestart") > 0, "the client is sent the death's ClientRestart");
    CHECK(calls_named(h, h.pawn_ch, from, "PlayerPawn_C", "ClientSetLastDeathLocation") > 0, "... and where it fell");
    const GmRunResult dead_again = run("kill", {{"character", "cyb"}});
    CHECK(dead_again.status == GmStatus::FailedPrecondition && dead_again.message.find("dead already") != std::string::npos, "killing a dead character is refused");
    const size_t before_revive = h.got.size();
    const GmRunResult revived = run("revive", {{"character", "cyb"}});
    CHECK(revived.status == GmStatus::Ok && !h.s->dead() && h.s->respawns() == 1, "revive brings it back (one respawn)");
    CHECK(revived.message.find("came back at") != std::string::npos && one_line(revived.message), "... at a named place, in one line");
    CHECK(near_to(stat_of(*h.s, content::kGuidHealth), stat_of(*h.s, content::kGuidMaxHealth), 0.01) &&
              near_to(stat_of(*h.s, content::kGuidMana), stat_of(*h.s, content::kGuidMaxMana), 0.01),
          "the revived character has full Health and Mana");
    h.run(milliseconds(100));
    CHECK(corrections_on(h, h.pawn_ch, before_revive, "PlayerPawn_C") > 0, "the client is sent the respawn correction");
    CHECK(run("revive", {{"character", "cyb"}}).status == GmStatus::FailedPrecondition, "reviving a living character is refused");
}
AOC_REGISTER_TEST(test_gm_admin_kill_and_revive);

static void test_gm_admin_give_experience() {
    std::printf("give-experience: the total, the level it crosses and the client's frames\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const auto run = [&](const char* command, std::map<std::string, std::string> fields) { return travel.run_command(command, fields, "the test"); };
    CHECK(h.s->progress().level == 1 && h.s->progress().xp == 0, "(a new character is at level 1, 0 XP)");
    const size_t from = h.got.size();
    const GmRunResult r = run("give-experience", {{"character", "cyb"}, {"amount", "350"}});
    CHECK(r.status == GmStatus::Ok && h.s->progress().xp == 350, "give-experience adds 350 XP");
    CHECK(h.s->progress().level == 2 && h.s->levels_gained() == 1, "it crosses level 2 (the content.db's curve reaches it at 300)");
    CHECK(r.message.find("level 2") != std::string::npos && one_line(r.message), "... and the answer says which level it reached");
    h.run(milliseconds(100));
    CHECK(!chains_on(h, h.pc_ch, from).empty() || !chains_on(h, h.pawn_ch, from).empty(), "the client is sent the experience and level frames");
    CHECK(run("give-experience", {{"character", "cyb"}, {"amount", "0"}}).status == GmStatus::InvalidArgument, "0 experience is outside the list's range");
    CHECK(run("give-experience", {{"character", "nobody"}, {"amount", "1"}}).status == GmStatus::NotFound, "an unknown character is NotFound");
}
AOC_REGISTER_TEST(test_gm_admin_give_experience);

static void test_gm_admin_clear_cooldowns() {
    std::printf("clear-cooldowns: a running cooldown ends and the pawn's Cooldowns follow\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    // the Dodge, triggered as the client triggers it on the pawn's ability component (a test character has not bought its rank: the GM teach gives it before the runtime is made)
    h.s->teach_ability(kDodgeAbility);
    h.play();
    GmTravel travel = make_travel(h);
    const auto run = [&](const char* command, std::map<std::string, std::string> fields) { return travel.run_command(command, fields, "the test"); };
    h.s->set_current_value(content::kGuidStamina, 100.0f);
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, h.s->bootstrap().setup().guids.ability, kDodgeAbility, 1), true)});
    h.run(milliseconds(300));
    CHECK(h.s->ability_triggers() > 0, "(the Dodge trigger reached the session)");
    CHECK(h.s->abilities() && h.s->abilities()->dodge_cooldown_until() >= 0, "(the Dodge started its cooldown)");
    const size_t from = h.got.size();
    const GmRunResult cleared = run("clear-cooldowns", {{"character", "cyb"}});
    CHECK(cleared.status == GmStatus::Ok && h.s->abilities()->dodge_cooldown_until() < 0 && h.s->pipeline()->cooldowns_running() == 0,
          "clear-cooldowns ends every running cooldown");
    CHECK(cleared.message.find("cooldown group") != std::string::npos, "... and says how many");
    h.step();
    CHECK(!chains_on(h, h.pawn_ch, from).empty(), "the client is sent the ability update with the Cooldowns removed");
    CHECK(run("clear-cooldowns", {{"character", "cyb"}}).status == GmStatus::FailedPrecondition, "a character with no cooldown gets a FailedPrecondition");
}
AOC_REGISTER_TEST(test_gm_admin_clear_cooldowns);

static void test_gm_admin_give_mount() {
    std::printf("give-mount: the mount item lands in the bag\n");
    const std::string mount_name = "Mount_Grounded_Horse_0";
    const int64_t mount_item = g_other_mount_record;
    CHECK(mount_item != 0, "(the real data has the horse mount)");
    if (mount_item == 0) return;
    std::vector<svc::InventoryItem> items = kit();
    items.erase(std::remove_if(items.begin(), items.end(), [](const svc::InventoryItem& item) { return item.item_record_id == g_other_mount_record; }), items.end());
    Harness h(cyb_record(items), on_the_ground, {{mount_item, mount_name}});   // (the bag is the kit's; the horse's item row is added to the small content.db)
    h.play();
    GmTravel travel = make_travel(h);
    const auto run = [&](const char* command, std::map<std::string, std::string> fields) { return travel.run_command(command, fields, "the test"); };
    const size_t before = h.s->inventory().items().size();
    const GmRunResult r = run("give-mount", {{"character", "cyb"}, {"mount", mount_name}});
    size_t count = 0;
    for (const svc::InventoryItem& item : h.s->inventory().items()) count += item.item_record_id == mount_item ? 1 : 0;
    CHECK(r.status == GmStatus::Ok && count == 1 && h.s->inventory().items().size() == before + 1, "give-mount puts the mount item in the bag");
    CHECK(r.message.find("summon it") != std::string::npos && one_line(r.message), "... and says it is summoned as any mount");
    CHECK(run("give-mount", {{"character", "cyb"}, {"mount", "NoSuchMount"}}).status == GmStatus::NotFound, "an unknown mount is NotFound");
    CHECK(run("give-mount", {{"character", "nobody"}, {"mount", mount_name}}).status == GmStatus::NotFound, "an unknown character is NotFound");
}
AOC_REGISTER_TEST(test_gm_admin_give_mount);

// ---- the quest commands ------------------------------------------------------------------------------------------------------------------------------------------

static void test_gm_admin_quest_commands() {
    std::printf("start-quest, complete-objective, complete-quest and reset-quests on the real quests\n");
    std::string why;
    QuestPick pick;
    const bool have = pick.load(&why);
    CHECK(have, "the real content.db has a quest with an awake first objective");
    if (!have) {
        std::printf("  [info] %s\n", why.c_str());
        return;
    }
    QuestStub api;
    qs::QuestWorker::Config wc;
    wc.poll_interval = milliseconds(20);
    auto worker = std::make_unique<qs::QuestWorker>(api, wc);
    svc::Character record = cyb_on_the_ground();
    record.quest_log = svc::SavedQuestLog{};
    Harness h(record, [&](PlayerWorld& w) {
        on_the_ground(w);
        w.set_dialogue(std::move(*pick.dialogue));
        w.set_quests(qs::QuestBook(std::move(*pick.quest_data)));
        w.set_quest_worker(worker.get());
    });
    h.play();
    GmTravel travel = make_travel(h);
    const auto run = [&](const char* command, std::map<std::string, std::string> fields) { return travel.run_command(command, fields, "the test"); };
    CHECK(h.s->quests_on(), "(the quest log loaded and the quests are on)");
    const size_t from = h.got.size();
    const GmRunResult started = run("start-quest", {{"character", "cyb"}, {"quest", pick.first.name}, {"bypass", "yes"}});
    CHECK(started.status == GmStatus::Ok && h.s->quest_log().find(pick.first.quest) != nullptr, "start-quest takes the quest (its rules bypassed)");
    CHECK(started.message.find("started " + pick.first.name) != std::string::npos && one_line(started.message), "... and answers one line");
    h.settle();
    CHECK(h.got.size() > from, "the client is sent the quest component's update");
    const GmRunResult objective = run("complete-objective", {{"character", "cyb"}, {"objective", std::to_string(pick.first.objective)}});
    CHECK(objective.status == GmStatus::Ok, "complete-objective completes the quest's active objective");
    const qs::QuestInstance* instance = h.s->quest_log().find(pick.first.quest);
    const bool completed_objective = instance && !instance->objectives.empty() && instance->objectives.front().completed;
    CHECK(completed_objective || h.s->quest_log().completions(pick.first.quest) > 0, "... the objective (or the quest it completes) is done");
    CHECK(run("complete-objective", {{"character", "cyb"}, {"objective", std::to_string(pick.first.objective)}}).status == GmStatus::FailedPrecondition,
          "a completed objective cannot be completed again");
    CHECK(run("complete-objective", {{"character", "cyb"}, {"objective", "42"}}).status == GmStatus::NotFound, "an objective no quest of the character has is NotFound");
    CHECK(run("complete-quest", {{"character", "cyb"}, {"quest", pick.second.name}}).status == GmStatus::NotFound, "completing a quest the character has not taken is NotFound");
    CHECK(run("start-quest", {{"character", "cyb"}, {"quest", pick.second.name}, {"bypass", "yes"}}).status == GmStatus::Ok, "(the second quest is taken)");
    const GmRunResult finished = run("complete-quest", {{"character", "cyb"}, {"quest", pick.second.name}});
    CHECK(finished.status == GmStatus::Ok && h.s->quest_log().completions(pick.second.quest) > 0, "complete-quest finishes it and counts the completion");
    CHECK(finished.message.find("completed " + pick.second.name) != std::string::npos, "... and answers one line");
    const size_t finished_before = h.s->quest_log().completed().size();
    CHECK(run("reset-quests", {{"character", "cyb"}}).status == GmStatus::Ok && finished_before > 0, "reset-quests forgets the finished quests");
    CHECK(h.s->quest_log().completed().empty(), "... the completed list is empty");
    CHECK(run("start-quest", {{"character", "cyb"}, {"quest", "No Such Quest"}, {"bypass", "yes"}}).status == GmStatus::NotFound, "an unknown quest is NotFound");
}
AOC_REGISTER_TEST(test_gm_admin_quest_commands);

// ---- the world commands -----------------------------------------------------------------------------------------------------------------------------------------

static void test_gm_admin_time_of_day() {
    std::printf("set-time: the world clock and the ClientSetTime the players get\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const auto run = [&](const char* command, std::map<std::string, std::string> fields) { return travel.run_command(command, fields, "the test"); };
    const size_t from = h.got.size();
    const GmRunResult evening = run("set-time", {{"hour", "20"}});
    CHECK(evening.status == GmStatus::Ok && near_to(h.fx.world->day_clock().hour(h.fx.world->world_clock()), 20.0, 0.01),
          "set-time puts the world clock at the hour asked for");
    CHECK(evening.message.find("8:00 PM") != std::string::npos && evening.message.find("1 player") != std::string::npos, "... and says the hour and how many players were told");
    h.run(milliseconds(100));
    const std::optional<double> game_time = last_clock_game_time(h, from);
    const auto hour_of = [](double t) { double hour = std::fmod(t, 86400.0) / 3600.0; return hour < 0 ? hour + 24 : hour; };
    CHECK(game_time && near_to(hour_of(*game_time), 20.0, 0.05), "the client's ClientSetTime carries the new game time");
    CHECK(run("set-time", {{"hour", "24"}}).status == GmStatus::Ok && near_to(h.fx.world->day_clock().hour(h.fx.world->world_clock()), 0.0, 0.01), "24 is midnight");
    CHECK(run("set-time", {{"hour", "25"}}).status == GmStatus::InvalidArgument, "an hour outside the list's range is refused");
    CHECK(run("set-time", {{"hour", "seven"}}).status == GmStatus::InvalidArgument, "an hour that is not a number is refused");
}
AOC_REGISTER_TEST(test_gm_admin_time_of_day);

namespace {

/// A node control that only remembers what it was asked (the node service itself is not this test's).
struct RecordingNodeControl : world::NodeControl {
    std::string node;
    int64_t     amount = 0;
    size_t      calls = 0;
    void set_level(const std::string&, int, bool, const std::string&) override {}
    void add_experience(const std::string& n, int64_t a, const std::string&) override {
        node = n;
        amount = a;
        ++calls;
    }
    void node_data(const std::string&, const std::string&) override {}
    void clock(const std::string&) override {}
    void advance_clock(int64_t, const std::string&) override {}
};

}  // namespace

static void test_gm_admin_node_xp() {
    std::printf("node-xp: the city node service is asked with the node and the amount\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    RecordingNodeControl control;
    h.fx.world->set_node_control(&control);
    GmTravel travel = make_travel(h);
    const GmRunResult r = travel.run_command("node-xp", {{"node", "Joeva"}, {"amount", "1200"}}, "the test");
    CHECK(r.status == GmStatus::Ok && control.calls == 1 && control.node == "Joeva" && control.amount == 1200, "node-xp asks the node service once, with the node and the amount");
    CHECK(r.message.find("1200") != std::string::npos && one_line(r.message), "... and answers one line");
    CHECK(travel.run_command("node-xp", {{"node", "Joeva"}, {"amount", "0"}}, "the test").status == GmStatus::InvalidArgument, "0 XP is outside the list's range");
    h.fx.world->set_node_control(nullptr);
    CHECK(travel.run_command("node-xp", {{"node", "Joeva"}, {"amount", "1"}}, "the test").status == GmStatus::FailedPrecondition, "a world without the node service refuses it");
}
AOC_REGISTER_TEST(test_gm_admin_node_xp);

// ---- the log ----------------------------------------------------------------------------------------------------------------------------------------------------

static void test_gm_admin_logging() {
    std::printf("the admin log: one line per run, with the command, the character, the fields and the result\n");
    std::ostringstream log;
    auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("gm_admin", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    {
        Harness h(cyb_on_the_ground(), on_the_ground);
        h.play();
        GmTravel travel = make_travel(h);
        log.str("");
        const GmRunResult ok = travel.run_command("heal", {{"character", "cyb"}}, "worldctl");
        const GmRunResult refused = travel.run_command("heal", {{"character", "nobody"}}, "worldctl");
        const GmRunResult forged = travel.run_command("teleport", {{"character", "cyb"}, {"place", "Atlantis\n[forged] a line of the caller's"}}, "worldctl");
        CHECK(ok.status == GmStatus::Ok && refused.status == GmStatus::NotFound && forged.status == GmStatus::NotFound, "(the three runs did what they were asked)");
        std::istringstream in(log.str());
        std::string line;
        size_t runs = 0, heal_lines = 0, forged_lines = 0, token_lines = 0, ok_results = 0, refused_results = 0, forged_start_lines = 0;
        while (std::getline(in, line)) {
            if (line.find("[admin] run ") == std::string::npos) continue;
            ++runs;
            if (line.find("[admin] run heal ") != std::string::npos) ++heal_lines;
            if (line.find("[admin] run teleport ") != std::string::npos) ++forged_lines;
            if (line.find(kToken) != std::string::npos) ++token_lines;
            if (line.find("healed to full Health and Mana") != std::string::npos) ++ok_results;
            if (line.find("not in the world") != std::string::npos) ++refused_results;
            if (line.rfind("[forged]", 0) == 0) ++forged_start_lines;
        }
        CHECK(runs == 3, "three runs write three [admin] run lines");
        CHECK(heal_lines == 2 && forged_lines == 1, "each line names the command");
        CHECK(log.str().find("character=\"cyb\"") != std::string::npos && log.str().find("character=\"nobody\"") != std::string::npos, "each line names the character");
        CHECK(ok_results == 1 && refused_results >= 1, "each line carries the result, the refusals too");
        CHECK(forged_start_lines == 0 && log.str().find("Atlantis?[forged]") != std::string::npos, "a control character in a field is shown as '?' and cannot start a line");
        CHECK(token_lines == 0 && log.str().find("Bearer") == std::string::npos, "no token or login value ever reaches the log");
    }
    spdlog::set_default_logger(previous);
}
AOC_REGISTER_TEST(test_gm_admin_logging);

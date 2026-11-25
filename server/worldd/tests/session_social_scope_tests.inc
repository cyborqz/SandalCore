// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_social_scope_tests.inc: the PlayerController's social component (the mail's calls)
//
//  The dispatch's sixth scope, RpcScope::SocialComponent: a non-actor call whose object is the PlayerController's social component reaches a handler
//  registered on that scope, once per call and from a second session as well.  A social-component call with no handler is still logged as a call
//  worldd does not answer and is answered with nothing.  The five scopes before it still dispatch their calls.  The reach test's probe is the test's
//  own handler, on a Client* call of the component that no feature registers, so the test means the same once the mail feature is merged.
// ============================================================================
#include <array>
#include <string>

namespace {

/// The calls the test's probe on the social component counted.
static size_t test_social_probe_calls = 0;

/// The test's handler on RpcScope::SocialComponent: a Client* call that no feature registers.
static void test_social_scope_probe(PlayerSession&, const RpcContext&) { ++test_social_probe_calls; }

/// Sends a call on a component: a non-actor block whose object is the component's GUID, on `role`'s channel, read back with the component's class.
static bool test_component_send(PartyWorld& w, size_t who, bs::Role role, const schema::ClassSchema& cls, const FIntrepidNetworkGUID& object, const char* name,
                                const repl::RpcParams& params = {}) {
    if (!w[who].s) return false;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return false;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) return false;
    BunchWriter out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.object = object;
    if (repl::write_content_block(out, header, field, /*server_form=*/false) == 0) return false;
    // The call reads back with the component's class before it is sent (the server's own decoder): a bunch that does not read would be ignored there.
    const auto parsed = repl::read_client_bunch(cls, [&](const emit::FIntrepidNetworkGUID&) { return &cls; }, out.data(), out.bit_pos(), {});
    if (!parsed || parsed->rpcs.size() != 1 || !parsed->rpcs[0].call.decoded) {
        std::printf("  [FAIL] the client's %s does not read back (%zu bits)\n", name, out.bit_pos());
        return false;
    }
    w.client_sends(w[who], {w[who].client.actor(w[who].s->channel_of(role), out, true)});
    return true;
}

/// Sends a call on the PlayerController's social component.
static bool test_social_send(PartyWorld& w, size_t who, const char* name, const repl::RpcParams& params = {}) {
    if (!w[who].s) return false;
    return test_component_send(w, who, bs::Role::PlayerController, class_named("UIntrepidSocialComponent"), w[who].s->bootstrap().setup().guids.social, name, params);
}

/// The four words of a fake mail GUID as the parameter `prefix` of the social component's calls.
static bool test_social_put_mail_guid(const schema::RpcSchema& rpc, repl::RpcParams& params, const char* prefix) {
    static const char* const kWord[4] = {"A", "B", "C", "D"};
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        ok = repl::set_rpc_param(rpc, params, std::string(prefix) + "." + kWord[i], repl::Value::i64(static_cast<int32_t>(0x00000061u + static_cast<uint32_t>(i)))) && ok;
    }
    return ok;
}

/// The log text the dispatch writes for a call it does not answer.
static std::string test_social_ignored_line(const char* name, const char* role) {
    return std::string("ignoring the client's ") + name + " (" + role + "): worldd does not answer it";
}

/// The calls on `object` in the chains player `who` was given from `from` on (the guild tests' reader, for the social component).
static size_t test_social_calls_to(const PartyWorld& w, size_t who, size_t from, const FIntrepidNetworkGUID& object) {
    size_t calls = 0;
    const schema::ClassSchema& cls = class_named("UIntrepidSocialComponent");
    for (size_t i = from; i < w.p[who].got.size(); ++i) {
        const fake::Delivered& d = w.p[who].got[i];
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& v : *blocks) {
            if (v.header.is_actor || v.header.has_rep_layout || !(v.header.object == object)) continue;
            size_t p = v.payload_pos;
            const auto found = repl::read_rpc_fields(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
            if (found) calls += found->size();
        }
    }
    return calls;
}

}  // namespace

static void test_a_call_on_the_social_component_reaches_its_handler() {
    std::printf("a call on the PlayerController's social component reaches a handler on the new scope, once per send and from a second session\n");
    std::fflush(stdout);
    PartyWorld w;
    if (!w.enter(0, party_record("cyb", {0x5101, 0, 0, 1}), identity())) {
        CHECK(false, "the session joins");
        return;
    }
    RpcTable* table = rpc_table_for_test();
    CHECK(table != nullptr, "the session built its table and the test can reach it");
    if (!table) return;
    CHECK(table->find(RpcScope::SocialComponent, "ClientUpdateMailStatus") == nullptr, "the social component's ClientUpdateMailStatus has no feature handler");
    table->add(RpcScope::SocialComponent, "ClientUpdateMailStatus", &test_social_scope_probe);
    CHECK(table->find(RpcScope::SocialComponent, "ClientUpdateMailStatus") == &test_social_scope_probe, "the test's probe is registered on RpcScope::SocialComponent");
    const schema::RpcSchema* update = class_named("UIntrepidSocialComponent").rpc("ClientUpdateMailStatus");
    repl::RpcParams params;
    CHECK(update && test_social_put_mail_guid(*update, params, "MailGuid") && repl::set_rpc_param(*update, params, "Status", repl::Value::u64(2)),
          "the probe's ClientUpdateMailStatus carries a fake MailGuid and Status");
    if (!update) return;
    test_social_probe_calls = 0;
    // (the log from just before the call: an earlier line of the same name cannot satisfy or spoil the check)
    size_t log_from = g_log.str().size();
    CHECK(test_social_send(w, 0, "ClientUpdateMailStatus", params), "ClientUpdateMailStatus is sent on the PlayerController's social component");
    w.settle();
    CHECK(g_log.str().substr(log_from).find(test_social_ignored_line("ClientUpdateMailStatus", "PlayerController")) == std::string::npos,
          "the call on the social component is not logged as a call worldd does not answer");
    CHECK(test_social_probe_calls == 1, "the call reached the probe registered on RpcScope::SocialComponent");
    log_from = g_log.str().size();
    CHECK(test_social_send(w, 0, "ClientUpdateMailStatus", params), "the same call is sent a second time");
    w.settle();
    CHECK(g_log.str().substr(log_from).find(test_social_ignored_line("ClientUpdateMailStatus", "PlayerController")) == std::string::npos,
          "the second call is not logged as a call worldd does not answer either");
    CHECK(test_social_probe_calls == 2, "the second call reached the probe");
    if (!w.enter(1, party_record("dee", {0x5102, 0, 0, 2}), identity())) {
        CHECK(false, "the second session joins");
        return;
    }
    log_from = g_log.str().size();
    CHECK(test_social_send(w, 1, "ClientUpdateMailStatus", params), "the second session sends the same call on its social component");
    w.settle();
    CHECK(g_log.str().substr(log_from).find(test_social_ignored_line("ClientUpdateMailStatus", "PlayerController")) == std::string::npos,
          "the second session's call is not logged as a call worldd does not answer");
    CHECK(test_social_probe_calls == 3, "the second session's call reached the same probe");
}

static void test_a_social_component_call_with_no_handler_is_logged() {
    std::printf("a social-component call with no handler is logged as worldd's unhandled call and answered with nothing\n");
    std::fflush(stdout);
    PartyWorld w;
    if (!w.enter(0, party_record("cyb", {0x5201, 0, 0, 1}), identity())) {
        CHECK(false, "the session joins");
        return;
    }
    const schema::RpcSchema* claim = class_named("UIntrepidSocialComponent").rpc("ClientClaimMailCurrency");
    repl::RpcParams params;
    CHECK(claim && test_social_put_mail_guid(*claim, params, "MailGuid"), "ClientClaimMailCurrency carries a fake MailGuid");
    const size_t from = w[0].got.size();
    const size_t log_from = g_log.str().size();
    CHECK(test_social_send(w, 0, "ClientClaimMailCurrency", params), "ClientClaimMailCurrency, which no feature registers, is sent on the social component");
    w.settle();
    CHECK(g_log.str().substr(log_from).find(test_social_ignored_line("ClientClaimMailCurrency", "PlayerController")) != std::string::npos,
          "the call with no handler is logged as a call worldd does not answer");
    CHECK(test_social_calls_to(w, 0, from, w[0].s->bootstrap().setup().guids.social) == 0,
          "the call with no handler is answered with nothing: no new chain carries a call on the social component");
}

static void test_the_five_existing_scopes_still_dispatch() {
    std::printf("the five scopes before the social one still dispatch their calls and are not logged as unanswered\n");
    std::fflush(stdout);
    PartyWorld w;
    if (!w.enter(0, party_record("cyb", {0x5301, 0, 0, 1}), identity())) {
        CHECK(false, "the session joins");
        return;
    }
    RpcTable* table = rpc_table_for_test();
    CHECK(table != nullptr, "the session built its table and the test can reach it");
    if (!table) return;
    const auto& setup = w[0].s->bootstrap().setup();
    const schema::ClassSchema& pawn_cls = class_named("PlayerPawn_C");
    // the PlayerController's own calls (an actor call on the PlayerController's channel, as party_send sends it)
    CHECK(table->find(RpcScope::PlayerController, "ServerCloseLoot") != nullptr, "ServerCloseLoot is registered on the PlayerController's scope");
    size_t log_from = g_log.str().size();
    CHECK(party_send(w, 0, "ServerCloseLoot", repl::RpcParams{}), "ServerCloseLoot is sent on the PlayerController");
    w.settle();
    CHECK(g_log.str().substr(log_from).find(test_social_ignored_line("ServerCloseLoot", "PlayerController")) == std::string::npos,
          "the PlayerController's call is dispatched, not logged as unanswered");
    // the pawn's own calls (an actor call on the pawn's channel)
    CHECK(table->find(RpcScope::Pawn, "ServerClientIsReadyToPhaseIn") != nullptr, "ServerClientIsReadyToPhaseIn is registered on the pawn's scope");
    const BunchWriter ready = client_rpc(pawn_cls, "ServerClientIsReadyToPhaseIn");
    CHECK(ready.bit_pos() != 0, "the pawn's ServerClientIsReadyToPhaseIn writes");
    log_from = g_log.str().size();
    w.client_sends(w[0], {w[0].client.actor(w[0].s->channel_of(bs::Role::Pawn), ready, true)});
    w.settle();
    CHECK(g_log.str().substr(log_from).find(test_social_ignored_line("ServerClientIsReadyToPhaseIn", "Pawn")) == std::string::npos,
          "the pawn's call is dispatched, not logged as unanswered");
    // the pawn's ability component
    CHECK(table->find(RpcScope::AbilityComponent, "ServerEndAbility") != nullptr, "ServerEndAbility is registered on the ability component's scope");
    log_from = g_log.str().size();
    CHECK(test_component_send(w, 0, bs::Role::Pawn, class_named("UAoCAbilityComponent"), setup.guids.ability, "ServerEndAbility"), "ServerEndAbility is sent on the ability component");
    w.settle();
    CHECK(g_log.str().substr(log_from).find(test_social_ignored_line("ServerEndAbility", "Pawn")) == std::string::npos,
          "the ability component's call is dispatched, not logged as unanswered");
    // the PlayerController's tutorial component
    CHECK(table->find(RpcScope::FtueComponent, "Server_RemoveLocaleFTUEs") != nullptr, "Server_RemoveLocaleFTUEs is registered on the tutorial component's scope");
    log_from = g_log.str().size();
    CHECK(test_component_send(w, 0, bs::Role::PlayerController, class_named("UFTUEComponent"), setup.guids.ftue, "Server_RemoveLocaleFTUEs"),
          "Server_RemoveLocaleFTUEs is sent on the tutorial component");
    w.settle();
    CHECK(g_log.str().substr(log_from).find(test_social_ignored_line("Server_RemoveLocaleFTUEs", "PlayerController")) == std::string::npos,
          "the tutorial component's call is dispatched, not logged as unanswered");
    // the pawn's guild component
    CHECK(table->find(RpcScope::GuildComponent, "ServerGetUpdatedGuildData") != nullptr, "ServerGetUpdatedGuildData is registered on the guild component's scope");
    log_from = g_log.str().size();
    CHECK(test_component_send(w, 0, bs::Role::Pawn, class_named("UIntrepidGuildComponent"), setup.guids.guild, "ServerGetUpdatedGuildData"),
          "ServerGetUpdatedGuildData is sent on the guild component");
    w.settle();
    CHECK(g_log.str().substr(log_from).find(test_social_ignored_line("ServerGetUpdatedGuildData", "Pawn")) == std::string::npos,
          "the guild component's call is dispatched, not logged as unanswered");
}

AOC_REGISTER_TEST(test_a_call_on_the_social_component_reaches_its_handler);
AOC_REGISTER_TEST(test_a_social_component_call_with_no_handler_is_logged);
AOC_REGISTER_TEST(test_the_five_existing_scopes_still_dispatch);

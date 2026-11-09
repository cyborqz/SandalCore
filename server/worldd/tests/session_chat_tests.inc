// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_chat_tests.inc
//
//  Registered tests of test_worldd_session: the first two social steps.  The server sends one welcome line in the chat window
//  once a session, after the phase-in, and never again in that session; a line the client sends with SendChatMessage comes
//  back to the sender as one ClientReceiveChatMessages with the client's category and the character's name.  An empty line and
//  a line over 512 characters are refused; item-link markup is passed through unparsed.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================
#include "protocol/repl/rpc_fields.h"

namespace {

/// One ClientReceiveChatMessages message as the client reads it.
struct ChatLine {
    std::string message, from, decoration;
    uint64_t    category = 0;
    bool        gm = true;
    bool        has_item = true;
    size_t      bits = 0;   // the chain's payload
};

/// The one message of a delivered PlayerController chain, when that chain is one ClientReceiveChatMessages.
static std::optional<ChatLine> chat_line_of(const fake::Delivered& d, const schema::ClassSchema& pc_cls) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks || blocks->size() != 1) return std::nullopt;
    size_t pos = (*blocks)[0].payload_pos;
    const auto calls = repl::read_rpc_fields(pc_cls, d.content.data(), pos + (*blocks)[0].payload_bits, pos);
    if (!calls || calls->size() != 1) return std::nullopt;
    const repl::RpcCall& call = (*calls)[0];
    if (!call.decoded || std::string(call.rpc->name) != "ClientReceiveChatMessages") return std::nullopt;
    const repl::Value* messages = repl::rpcf::value_of(*call.rpc, call.params, "Messages");
    const auto* list = messages ? messages->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.size() != 1) return std::nullopt;
    const repl::ElementState& e = list->elements.front();
    ChatLine line;
    line.bits = d.content.bit_pos();
    const repl::Value* message = repl::rpcf::element_value(e, 1);
    const repl::Value* from = repl::rpcf::element_value(e, 2);
    const repl::Value* category = repl::rpcf::element_value(e, 3);
    const repl::Value* decoration = repl::rpcf::element_value(e, 4);
    const repl::Value* gm = repl::rpcf::element_value(e, 5);
    const repl::Value* item = repl::rpcf::element_value(e, 6);
    const auto* m = message ? message->get_if<std::string>() : nullptr;
    const auto* f = from ? from->get_if<std::string>() : nullptr;
    const auto* c = category ? category->get_if<uint64_t>() : nullptr;
    const auto* dec = decoration ? decoration->get_if<std::string>() : nullptr;
    const auto* g = gm ? gm->get_if<bool>() : nullptr;
    const auto* it = item ? item->get_if<repl::ItemInstance>() : nullptr;
    if (!m || !f || !c || !dec || !g || !it) return std::nullopt;
    line.message = *m;
    line.from = *f;
    line.category = *c;
    line.decoration = *dec;
    line.gm = *g;
    line.has_item = it->has_item;
    return line;
}

/// Every chat line the client was given from chain `from` on.
static std::vector<ChatLine> chat_lines(const Harness& h, const schema::ClassSchema& pc_cls, size_t from) {
    std::vector<ChatLine> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (auto line = chat_line_of(h.got[i], pc_cls)) out.push_back(std::move(*line));
    }
    return out;
}

/// A client SendChatMessage with every member of its FChatData set: `quote` is the line, `category` the client's own.
static BunchWriter chat_call(const schema::ClassSchema& pc_cls, const std::string& quote, uint64_t category) {
    const schema::RpcSchema* rpc = pc_cls.rpc("SendChatMessage");
    repl::RpcParams params;
    bool ok = rpc != nullptr;
    auto put = [&](const char* path, repl::Value v) {
        if (ok) ok = repl::set_rpc_param(*rpc, params, path, std::move(v));
    };
    put("ChatData.m_bIsSelf", repl::Value::boolean(false));
    put("ChatData.SubjectName", repl::Value::str("cyb"));
    put("ChatData.Subject", repl::Value(repl::NameValue::hardcoded_name(0)));
    put("ChatData.TargetName", repl::Value::str(std::string()));
    put("ChatData.TargetGender", repl::Value::u64(0));
    put("ChatData.Target", repl::Value(repl::NameValue::hardcoded_name(0)));
    put("ChatData.Verb2nd", repl::Value(repl::TextValue{}));
    put("ChatData.Verb3rd", repl::Value(repl::TextValue{}));
    put("ChatData.Quote", repl::Value::str(quote));
    put("ChatData.Language", repl::Value(repl::NameValue::hardcoded_name(0)));
    put("ChatData.bCanLanguageBeUnderstood", repl::Value::boolean(true));
    put("ChatData.ChatCategory", repl::Value::u64(category));
    put("ChatData.DistanceSquared", repl::Value::f32(0.0f));
    put("ChatData.bSentFromGM", repl::Value::boolean(false));
    put("ChatData.LocationX", repl::Value::f32(0.0f));
    put("ChatData.LocationY", repl::Value::f32(0.0f));
    put("ChatData.EmblemDecorators", repl::Value::str(std::string()));
    put("ChatData.ItemAssociatedData", repl::Value(repl::ItemInstance{}));
    if (!ok) return BunchWriter{};
    return client_rpc(pc_cls, "SendChatMessage", params);
}

/// A joined session that has entered the world (its phase-in sent and everything settled).
static bool enter_world(Harness& h) {
    if (!h.join() || !h.s || h.s->failed()) return false;
    h.settle();
    h.client_sends({h.client.actor(h.s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    return true;
}

}  // namespace

static void test_chat_encoding() {
    std::printf("the welcome message is one recorded-shape chat line and decodes to its six fields\n");
    Harness h;
    CHECK(h.join() && !h.s->failed(), "the session joins");
    h.settle();
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(h.s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    const auto lines = chat_lines(h, pc_cls, before);
    CHECK(lines.size() == 1, "the client gets one ClientReceiveChatMessages");
    if (lines.size() != 1) return;
    const ChatLine& line = lines[0];
    // A recorded one-line answer is 351 bits for 21 string characters (a 13-character name and an 8-character line); this line
    // has 28 (6 + 22), so the same frame is 351 + 7 * 8 = 407 bits.
    std::printf("    the welcome chain is %zu bits\n", line.bits);
    CHECK(line.bits == 407, "one message of this shape is 407 bits: the recorded answer's 351 bits plus 7 characters");
    CHECK(line.message == "Welcome to SandalCore." && line.from == "Server" && line.category == 124 &&
              line.decoration.empty() && !line.gm && !line.has_item,
          "it decodes back to the welcome, From Server, category SYSBROADCAST (124), empty Decoration, bGM false, an empty item");
}

AOC_REGISTER_TEST(test_chat_encoding);

static void test_welcome_once() {
    std::printf("the welcome line once a session, none on a zone change, once again in a new session\n");
    Harness h;
    CHECK(h.join() && !h.s->failed(), "the session joins");
    if (!h.s) return;
    h.settle();
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    const auto first = chat_lines(h, pc_cls, before);
    CHECK(first.size() == 1 && first[0].message == "Welcome to SandalCore." && first[0].from == "Server" && first[0].category == 124,
          "a session that enters the world gets exactly one welcome line from Server (SYSBROADCAST)");
    const size_t after_phase_in = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    CHECK(chat_lines(h, pc_cls, after_phase_in).empty(), "the client ready again (a zone change) sends none");

    Harness again;
    CHECK(again.join() && !again.s->failed(), "a new session joins");
    again.settle();
    const size_t before_again = again.got.size();
    again.client_sends({again.client.actor(again.s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    again.settle();
    const auto lines = chat_lines(again, pc_cls, before_again);
    CHECK(lines.size() == 1 && lines[0].message == "Welcome to SandalCore." && lines[0].from == "Server",
          "a new session sends the welcome line again");
}

AOC_REGISTER_TEST(test_welcome_once);

static void test_say_echo() {
    std::printf("a /say line comes back from the server with the client's category and the character's name\n");
    Harness h;
    if (!enter_world(h)) { CHECK(false, "the session joins and enters the world"); return; }
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, chat_call(pc_cls, "hi", 18), true)});
    h.settle();
    const auto lines = chat_lines(h, pc_cls, before);
    CHECK(lines.size() == 1, "one SendChatMessage is answered with exactly one ClientReceiveChatMessages");
    if (lines.size() == 1) {
        const ChatLine& line = lines[0];
        CHECK(line.message == "hi", "the text is the one that was sent");
        CHECK(line.from == h.s->name() && line.from == "cyb", "From is the sender's character name");
        CHECK(line.category == 18, "the client's own category (18) comes back unchanged");
        CHECK(line.decoration.empty() && !line.gm && !line.has_item, "Decoration empty, bGM false, an empty item");
    }
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("category 18") != std::string::npos, "the category number is logged");
}

AOC_REGISTER_TEST(test_say_echo);

static void test_say_refusals() {
    std::printf("an empty, whitespace-only and over-long line are refused with nothing sent\n");
    Harness h;
    if (!enter_world(h)) { CHECK(false, "the session joins and enters the world"); return; }
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    size_t before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, chat_call(pc_cls, "", 18), true)});
    h.settle();
    CHECK(chat_lines(h, pc_cls, before).empty(), "an empty line gets nothing");
    before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, chat_call(pc_cls, "   \t ", 18), true)});
    h.settle();
    CHECK(chat_lines(h, pc_cls, before).empty(), "a whitespace-only line gets nothing");
    before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, chat_call(pc_cls, std::string(513, 'x'), 18), true)});
    h.settle();
    CHECK(chat_lines(h, pc_cls, before).empty(), "a line of 513 characters gets nothing");
    before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, chat_call(pc_cls, std::string(512, 'x'), 18), true)});
    h.settle();
    CHECK(chat_lines(h, pc_cls, before).size() == 1, "a line of 512 characters (the limit) comes back");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("not echoed") != std::string::npos, "the refusals are logged");
}

AOC_REGISTER_TEST(test_say_refusals);

static void test_say_item_link() {
    std::printf("item-link markup in a /say line comes back unchanged\n");
    Harness h;
    if (!enter_world(h)) { CHECK(false, "the session joins and enters the world"); return; }
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const std::string link =
        "<a id=\"Item\" style=\"Item_Epic\" metaType=\"ItemLink\" metaGUID=\"6064633023086460928\" metaItemQuality=\"51\" "
        "metaEnchantLevel=\"6\" metaTemperingPercent=\"0\" metaDurability=\"22\">[Dustbound Ring +6]</> ";
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, chat_call(pc_cls, link, 18), true)});
    h.settle();
    const auto lines = chat_lines(h, pc_cls, before);
    CHECK(lines.size() == 1, "the linked line comes back once");
    CHECK(lines.size() == 1 && lines[0].message == link, "the markup is passed through, not parsed");
    CHECK(lines.size() == 1 && lines[0].category == 18 && lines[0].from == h.s->name(), "with the client's category and the character's name");
}

AOC_REGISTER_TEST(test_say_item_link);

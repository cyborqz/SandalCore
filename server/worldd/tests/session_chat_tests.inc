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
static std::vector<ChatLine> chat_lines_in(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from) {
    std::vector<ChatLine> out;
    for (size_t i = from; i < got.size(); ++i) {
        if (auto line = chat_line_of(got[i], pc_cls)) out.push_back(std::move(*line));
    }
    return out;
}

/// Every chat line the client was given from chain `from` on.
static std::vector<ChatLine> chat_lines(const Harness& h, const schema::ClassSchema& pc_cls, size_t from) {
    return chat_lines_in(h.got, pc_cls, from);
}

/// A client SendChatMessage with every member of its FChatData set: `quote` is the line, `category` the client's own and
/// `target` what the client put in TargetName (empty for a say).
static BunchWriter chat_call(const schema::ClassSchema& pc_cls, const std::string& quote, uint64_t category,
                             const std::string& target = std::string()) {
    const schema::RpcSchema* rpc = pc_cls.rpc("SendChatMessage");
    repl::RpcParams params;
    bool ok = rpc != nullptr;
    auto put = [&](const char* path, repl::Value v) {
        if (ok) ok = repl::set_rpc_param(*rpc, params, path, std::move(v));
    };
    put("ChatData.m_bIsSelf", repl::Value::boolean(false));
    put("ChatData.SubjectName", repl::Value::str("cyb"));
    put("ChatData.Subject", repl::Value(repl::NameValue::hardcoded_name(0)));
    put("ChatData.TargetName", repl::Value::str(target));
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

/// Two or three sessions on one world, each with its own connection and client and ticked together: what a /say and a
/// /tell reach (the chat directory of session/player_chat.cpp).
struct ChatWorld {
    Fixture fx;
    Clock::time_point t = t0;
    struct Player {
        Connection conn{ConnectionConfig{kOuter, kCf, kServerSeq, kClientSeq}, t0};
        fake::Client client{kOuter, kCf, kServerSeq, kClientSeq};
        std::unique_ptr<PlayerSession> s;
        std::vector<fake::Delivered> got;
        std::string error;
    };
    std::array<Player, 3> p;

    Player& operator[](size_t i) { return p[i]; }

    bool join(size_t i, const svc::Character& rec, const bs::LoginIdentity& id) {
        p[i].s = PlayerSession::join(p[i].conn, *fx.world, rec, id, world_time_at(t), p[i].error);
        return p[i].s != nullptr;
    }
    void take(Player& pl, const fake::Received& r) {
        for (const auto& d : r.delivered) pl.got.push_back(d);
    }
    void flush(Player& pl) {
        for (auto& pkt : pl.conn.flush(t)) take(pl, pl.client.receive(pkt));
    }
    void client_sends(Player& pl, std::vector<fake::Bunch> bunches) {
        const auto pkt = pl.client.packet(std::move(bunches));
        const std::vector<InBunch> in = pl.conn.receive(pkt, t);
        pl.s->take_acks(pl.conn);
        for (const InBunch& b : in) pl.s->on_bunch(pl.conn, b, t);
    }
    /// 20 ms: every session's full tick (the chat directory learns each session's connection there), then the clients
    /// receive and acknowledge.
    void step() {
        t += milliseconds(20);
        for (Player& pl : p) {
            if (!pl.s) continue;
            pl.s->tick(pl.conn, t);
            flush(pl);
            pl.s->take_acks(pl.conn);
        }
        for (Player& pl : p) {
            if (!pl.s) continue;
            client_sends(pl, {});
        }
    }
    size_t total_got() const {
        size_t n = 0;
        for (const Player& pl : p) n += pl.got.size();
        return n;
    }
    size_t pending() const {
        size_t n = 0;
        for (const Player& pl : p) {
            if (pl.s) n += pl.s->join_pending() + pl.s->replies_pending();
        }
        return n;
    }
    /// Steps until nothing is pending and a step brings nothing new.
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = total_got();
            step();
            quiet = total_got() == before && pending() == 0 ? quiet + 1 : 0;
        }
    }
    /// Joins player `i`, settles the join and sends its phase-in ready.
    bool enter(size_t i, const svc::Character& rec, const bs::LoginIdentity& id) {
        if (!join(i, rec, id) || !p[i].s || p[i].s->failed()) return false;
        settle();
        client_sends(p[i], {p[i].client.actor(p[i].s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
        settle();
        return true;
    }
};

/// A character of the chat tests at `(x, y, z)` cm: the join puts the player there.
static svc::Character chat_record(const char* name, const char* id, double x, double y, double z) {
    svc::Character c = cyb_record();
    c.id = id;
    c.name = name;
    c.has_position = true;
    c.position = {x, y, z, 0.0f};
    return c;
}

/// The spawn point the chat tests measure their distances from.
static constexpr double kChatX = bs::kDefaultSpawn.x, kChatY = bs::kDefaultSpawn.y, kChatZ = bs::kDefaultSpawn.z;

/// Moves player `i` to `(x, y, z)` cm (the path a GM teleport uses: the move checker's good position and a correction).
static bool chat_move(ChatWorld& w, size_t i, double x, double y, double z) {
    const PlayerSession::GmMoveResult r = w[i].s->gm_teleport(w[i].conn, x, y, z, w.t, "the chat test");
    w.settle();
    return r.status == PlayerSession::GmMoveResult::Status::Moved;
}

/// True when `line` is the chat line `text` from `from` in `category`, with empty Decoration, bGM false and an empty item.
static bool line_is(const ChatLine& line, const std::string& text, const std::string& from, uint64_t category, std::string* why) {
    if (line.message != text || line.from != from || line.category != category) {
        *why = "the line is \"" + line.message + "\" from \"" + line.from + "\", category " + std::to_string(line.category);
        return false;
    }
    if (!line.decoration.empty() || line.gm || line.has_item) {
        *why = "the line's Decoration is not empty, or bGM is true, or the item is not empty";
        return false;
    }
    return true;
}

/// The one line of a player's chat lines (the test's own: a say, an echo, a tell copy), or a failure sentence.
static bool one_line(const std::vector<ChatLine>& lines, const std::string& text, const std::string& from, uint64_t category, std::string* why) {
    if (lines.size() != 1) {
        *why = "the client got " + std::to_string(lines.size()) + " chat line(s), not one";
        return false;
    }
    return line_is(lines[0], text, from, category, why);
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

static void test_say_radius_two_players() {
    std::printf("a /say reaches the other player within 30 m on the ground and no player farther away\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    struct Case { const char* what; double dx; bool heard; };
    const Case cases[] = {{"5 m", 500.0, true}, {"29.9 m", 2990.0, true}, {"30.1 m", 3010.0, false}, {"60 m", 6000.0, false}};
    for (const Case& c : cases) {
        const std::string at = std::string("the other player at ") + c.what;
        CHECK(chat_move(w, 1, kChatX + c.dx, kChatY, kChatZ), ("the other player moves to " + std::string(c.what)).c_str());
        const std::string text = std::string("say at ") + c.what;
        const size_t before_a = w[0].got.size(), before_b = w[1].got.size();
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, text, 9), true)});
        w.settle();
        std::string why;
        CHECK(one_line(chat_lines_in(w[0].got, pc_cls, before_a), text, "cyb", 9, &why),
              ("the sender keeps its echo at " + std::string(c.what) + ": " + why).c_str());
        const auto heard = chat_lines_in(w[1].got, pc_cls, before_b);
        if (c.heard) {
            why.clear();
            CHECK(one_line(heard, text, "cyb", 9, &why), (at + " hears the line: " + why).c_str());
        } else {
            CHECK(heard.empty(), (at + " hears nothing").c_str());
        }
    }
}

AOC_REGISTER_TEST(test_say_radius_two_players);

static void test_say_radius_three_players() {
    std::printf("a /say reaches the two other players by their own distance: one inside 30 m, one outside\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 2990, kChatY, kChatZ), identity()) &&
              w.enter(2, chat_record("eve", "aaaabbbbccccddddeeeeffff00001111", kChatX + 3010, kChatY, kChatZ), identity()),
          "three players join and enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    {
        const size_t ba = w[0].got.size(), bb = w[1].got.size(), bc = w[2].got.size();
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "one", 9), true)});
        w.settle();
        std::string why;
        CHECK(one_line(chat_lines_in(w[0].got, pc_cls, ba), "one", "cyb", 9, &why), ("the sender keeps its echo: " + why).c_str());
        CHECK(one_line(chat_lines_in(w[1].got, pc_cls, bb), "one", "cyb", 9, &why), ("the player at 29.9 m hears the line: " + why).c_str());
        CHECK(chat_lines_in(w[2].got, pc_cls, bc).empty(), "the player at 30.1 m hears nothing");
    }
    CHECK(chat_move(w, 1, kChatX + 6000, kChatY, kChatZ) && chat_move(w, 2, kChatX + 500, kChatY, kChatZ), "the two players swap their places");
    {
        const size_t ba = w[0].got.size(), bb = w[1].got.size(), bc = w[2].got.size();
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "two", 9), true)});
        w.settle();
        std::string why;
        CHECK(one_line(chat_lines_in(w[0].got, pc_cls, ba), "two", "cyb", 9, &why), ("the sender keeps its echo: " + why).c_str());
        CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), "the player at 60 m hears nothing");
        CHECK(one_line(chat_lines_in(w[2].got, pc_cls, bc), "two", "cyb", 9, &why), ("the player at 5 m hears the line: " + why).c_str());
    }
}

AOC_REGISTER_TEST(test_say_radius_three_players);

static void test_say_vertical_distance_ignored() {
    std::printf("the /say radius is horizontal: height does not count\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ + 10000), identity()) &&
              w.enter(2, chat_record("eve", "aaaabbbbccccddddeeeeffff00001111", kChatX + 6000, kChatY, kChatZ + 10000), identity()),
          "three players join and enter the world, two of them 100 m above the speaker");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size(), bc = w[2].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "up", 9), true)});
    w.settle();
    std::string why;
    CHECK(one_line(chat_lines_in(w[0].got, pc_cls, ba), "up", "cyb", 9, &why), ("the sender keeps its echo: " + why).c_str());
    CHECK(one_line(chat_lines_in(w[1].got, pc_cls, bb), "up", "cyb", 9, &why), ("the player 5 m away and 100 m up hears the line: " + why).c_str());
    CHECK(chat_lines_in(w[2].got, pc_cls, bc).empty(), "the player 60 m away and 100 m up hears nothing");
}

AOC_REGISTER_TEST(test_say_vertical_distance_ignored);

static void test_say_other_categories_stay_with_the_sender() {
    std::printf("every chat category but 9 keeps the step-2 echo to the sender only\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world, 5 m apart");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const uint64_t categories[] = {10, 18, 31, 49, 50, 54, 55, 124};
    for (const uint64_t category : categories) {
        const std::string text = "category " + std::to_string(category);
        const size_t ba = w[0].got.size(), bb = w[1].got.size();
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, text, category), true)});
        w.settle();
        std::string why;
        CHECK(one_line(chat_lines_in(w[0].got, pc_cls, ba), text, "cyb", category, &why),
              ("the sender keeps the echo of category " + std::to_string(category) + ": " + why).c_str());
        CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), ("the player 5 m away hears nothing of category " + std::to_string(category)).c_str());
    }
}

AOC_REGISTER_TEST(test_say_other_categories_stay_with_the_sender);

static void test_tell_both_copies() {
    std::printf("a /tell: the sender reads \"To <name>: <text>\" and the player named reads the text under the sender's name\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()) &&
              w.enter(2, chat_record("eve", "aaaabbbbccccddddeeeeffff00001111", kChatX + 500, kChatY, kChatZ), identity()),
          "three players join and enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size(), bc = w[2].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "dee"), true)});
    w.settle();
    const auto sender = chat_lines_in(w[0].got, pc_cls, ba);
    const auto target = chat_lines_in(w[1].got, pc_cls, bb);
    CHECK(sender.size() == 1, "the sender gets exactly one copy");
    std::string why;
    if (sender.size() == 1) {
        CHECK(line_is(sender[0], "To dee: hi", "", 45, &why), ("the sender's copy is \"To dee: hi\" with From empty, category 45: " + why).c_str());
    }
    CHECK(target.size() == 1, "the player named gets exactly one copy");
    if (target.size() == 1) {
        CHECK(line_is(target[0], "hi", "cyb", 45, &why), ("the player named reads the text under the sender's name, category 45: " + why).c_str());
    }
    CHECK(chat_lines_in(w[2].got, pc_cls, bc).empty(), "the player 5 m away who was not named gets nothing");
}

AOC_REGISTER_TEST(test_tell_both_copies);

static void test_tell_to_self() {
    std::printf("a /tell to oneself sends both copies\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "self", 45, "cyb"), true)});
    w.settle();
    const auto lines = chat_lines_in(w[0].got, pc_cls, ba);
    CHECK(lines.size() == 2, "the sender reads two lines");
    std::string why;
    if (lines.size() == 2) {
        CHECK(line_is(lines[0], "To cyb: self", "", 45, &why), ("the first is the sender's copy \"To cyb: self\" with From empty: " + why).c_str());
        CHECK(line_is(lines[1], "self", "cyb", 45, &why), ("the second is the target's copy \"self\" from cyb: " + why).c_str());
    }
    CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), "the other player gets nothing");
}

AOC_REGISTER_TEST(test_tell_to_self);

static void test_tell_offline() {
    std::printf("a /tell to a name that is offline sends the sender's copy and one SYSBROADCAST line\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "Nobody_xyz"), true)});
    w.settle();
    const auto lines = chat_lines_in(w[0].got, pc_cls, ba);
    CHECK(lines.size() == 2, "the sender reads two lines");
    std::string why;
    if (lines.size() == 2) {
        CHECK(line_is(lines[0], "To Nobody_xyz: hi", "", 45, &why), ("the first is the sender's copy: " + why).c_str());
        CHECK(line_is(lines[1], "Nobody_xyz is not online.", "Server", 124, &why),
              ("the second is the SYSBROADCAST line \"Nobody_xyz is not online.\" from Server: " + why).c_str());
    }
    CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), "the other player gets nothing");
}

AOC_REGISTER_TEST(test_tell_offline);

static void test_tell_case_and_no_range() {
    std::printf("a /tell matches a name in any case, uses the name as sent and has no range\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 20000, kChatY, kChatZ), identity()),
          "two players join and enter the world, 200 m apart");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "DEE"), true)});
    w.settle();
    const auto sender = chat_lines_in(w[0].got, pc_cls, ba);
    CHECK(sender.size() == 1 && sender[0].message == "To DEE: hi" && sender[0].from.empty() && sender[0].category == 45,
          "the sender's copy uses the name as sent (DEE), with From empty, category 45");
    const auto target = chat_lines_in(w[1].got, pc_cls, bb);
    std::string why;
    CHECK(one_line(target, "hi", "cyb", 45, &why), ("the player named DEE (dee) 200 m away reads the text: " + why).c_str());
}

AOC_REGISTER_TEST(test_tell_case_and_no_range);

static void test_tell_text_cap() {
    std::printf("a /tell's text is capped at 512 characters like the say\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const std::string long_line(512, 'x');
    size_t ba = w[0].got.size(), bb = w[1].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, long_line, 45, "dee"), true)});
    w.settle();
    const auto sender = chat_lines_in(w[0].got, pc_cls, ba);
    CHECK(sender.size() == 1 && sender[0].message == "To dee: " + long_line, "a tell of 512 characters goes through to the sender");
    const auto target = chat_lines_in(w[1].got, pc_cls, bb);
    CHECK(target.size() == 1 && target[0].message == long_line, "the player named reads the 512 characters");
    ba = w[0].got.size();
    bb = w[1].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, std::string(513, 'x'), 45, "dee"), true)});
    w.settle();
    CHECK(chat_lines_in(w[0].got, pc_cls, ba).empty() && chat_lines_in(w[1].got, pc_cls, bb).empty(),
          "a tell of 513 characters sends nothing to either player");
}

AOC_REGISTER_TEST(test_tell_text_cap);

static void test_tell_name_with_space() {
    std::printf("a /tell to a name with a space uses the name as sent and finds no player\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "Dee Smith"), true)});
    w.settle();
    const auto lines = chat_lines_in(w[0].got, pc_cls, ba);
    CHECK(lines.size() == 2, "the sender reads two lines");
    std::string why;
    if (lines.size() == 2) {
        CHECK(line_is(lines[0], "To Dee Smith: hi", "", 45, &why), ("the sender's copy uses the name as sent: " + why).c_str());
        CHECK(line_is(lines[1], "Dee Smith is not online.", "Server", 124, &why), ("the SYSBROADCAST line names it as sent: " + why).c_str());
    }
    CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), "the player named dee gets nothing");
}

AOC_REGISTER_TEST(test_tell_name_with_space);

static void test_tell_with_no_name() {
    std::printf("a /tell with no name is refused\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, ""), true)});
    w.settle();
    CHECK(chat_lines_in(w[0].got, pc_cls, ba).empty(), "the sender reads nothing");
    CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), "the other player reads nothing");
}

AOC_REGISTER_TEST(test_tell_with_no_name);

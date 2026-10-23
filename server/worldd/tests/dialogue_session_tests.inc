// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/dialogue_session_tests.inc  (included by test_worldd_area_session.cpp)
//
//  Quests, slice 0: the talking window, on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB; the test skips without it).
//
//    * The book: every node, line and root of the data reads; every id a line leads to is a node of the data; a node with three lines has exactly three.
//    * The Town Crier's lines for a character with no quests (hand-read in the notes): which show, and what changes when the character is a citizen or stands elsewhere.
//    * A whole conversation through the real session, the real actor view and the fake client: the click opens the window, a line leads on, the lines that have no way
//      on do nothing, the client's close clears it once.  The controller's blocks are read with the repo's block reader and checked against the client's rules: every
//      id exists, every line names the node shown and an index below its count, the source is the NPC the connection has open and a close writes it back to nothing.
//    * Against the recordings (AOC_RECORDINGS_DIR, dialogue_blocks.txt: skipped without): the same conversation as a recorded one and a recorded vendor's greeting give
//      the recorded blocks bit for bit, the NPC's object reference apart.
//    * Hostile calls change nothing: a stale node, a line that is not shown, a call with no window, two clicks, the NPC gone, the player dead.
// ============================================================================
#include "dialogue/talk.h"
#include "protocol/repl/dialogue_rpc.h"

namespace dl = aoc::worldd::dialogue;

/// A string of bits, LSB first as on the wire: what the blocks are compared by.
struct DlgBits {
    std::vector<bool> b;
    size_t size() const { return b.size(); }
    bool operator==(const DlgBits& o) const { return b == o.b; }
    DlgBits slice(size_t at, size_t n) const { DlgBits r; r.b.assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n)); return r; }
    void erase(size_t at, size_t n) { b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n)); }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out((b.size() + 7) / 8 + 1, 0);
        for (size_t i = 0; i < b.size(); ++i) out[i >> 3] |= static_cast<uint8_t>(b[i] ? 1 : 0) << (i & 7);
        return out;
    }
    static DlgBits of_hex(const std::string& hex, size_t n) {
        DlgBits r;
        for (size_t i = 0; i < n; ++i) {
            const size_t at = (i >> 3) * 2;
            if (at + 1 >= hex.size() + 1) break;
            const auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            const int byte = nib(hex[at]) << 4 | nib(hex[at + 1]);
            r.b.push_back(((byte >> (i & 7)) & 1) != 0);
        }
        return r;
    }
    static DlgBits of_writer(const BunchWriter& w) {
        DlgBits r;
        const auto& bytes = w.bytes();
        for (size_t i = 0; i < w.bit_pos(); ++i) r.b.push_back(((bytes[i >> 3] >> (i & 7)) & 1) != 0);
        return r;
    }
};

// ---- the data ---------------------------------------------------------------------------------------------------------------------

static std::string dialogue_db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
    for (const char* p : {"dist/content/content.db", "../content/content.db", "../../dist/content/content.db"}) {
        if (fs::exists(p)) return p;
    }
    return {};
}

/// The conversations of the real content.db; nullopt (with the reason) when there is no file or it is older than contentc version 17.
static std::optional<dl::Book> dialogue_book(std::string& why) {
    const std::string path = dialogue_db_path();
    if (path.empty()) { why = "no content.db (dist/content/content.db or AOC_CONTENT_DB)"; return std::nullopt; }
    auto db = content::ContentDb::open(path, why);
    if (!db) return std::nullopt;
    auto data = db->dialogue_data(why);
    if (!data) return std::nullopt;
    return dl::Book(std::move(*data));
}

static int64_t root_with_top(const dl::Book& book, int64_t top) {
    for (const auto& [root, t] : book.data().roots) {
        if (t == top) return root;
    }
    return 0;
}

constexpr int64_t kTownCrierRoot = 6064634349883577684ll;      // the Town Crier's conversation
constexpr int64_t kTownCrierGreeting = 6064634349883577686ll;  // the node it shows: nine lines
constexpr int64_t kLionhold = 6064630651307950121ll;           // the node record the first line is hidden in

/// What the evaluator asks of a character who stands in `node`, is a citizen or not, and talks to an NPC in `npc_node`.
struct CrierFacts : dl::Facts {
    int64_t node = 6064630651307950001ll, npc_node = 6064630651307950001ll;
    bool    citizen = false;
    std::optional<dl::ExprValue> call(const std::string& name, const std::vector<dl::ExprValue>& args) const override {
        if (name == "GetPlayer") return dl::ExprValue::of(1);
        if (name == "GetInteractionTarget") return dl::ExprValue::of(2);
        if (name == "GetCurrentNode") return dl::ExprValue::of(node);
        if (name == "Node_GetActorCityNode") return dl::ExprValue::of(!args.empty() && args[0].i == 2 ? npc_node : node);
        if (name == "Node_IsPlayerCitizen") return dl::ExprValue::of(citizen ? 1 : 0);
        if (name.rfind("Node_", 0) == 0) return dl::ExprValue::of(0);   // buildings under construction, elections, expansions: none
        return std::nullopt;
    }
};

static std::vector<int32_t> line_indexes(const dl::Talk& t) {
    std::vector<int32_t> out;
    for (const dl::Talk::Line& l : t.state().lines) out.push_back(l.index);
    return out;
}

static repl::DialogueResponse choice_of(int64_t node, int32_t index) {
    repl::DialogueResponse r;
    r.response_type = static_cast<uint8_t>(repl::DialogueResponseType::Choice);
    r.node = node;
    r.node_type_id = repl::kDialogueNodeTypeId;
    r.choice_index = index;
    return r;
}

static void test_dialogue_book() {
    std::printf("the conversations of the data\n");
    std::string why;
    const auto book = dialogue_book(why);
    if (!book) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    CHECK(book->roots() == 1190 && book->nodes() == 5317, "the book holds the data's 1,190 roots and 5,317 nodes");
    size_t lines = 0, gated = 0, dangling = 0, empty_automatic = 0;
    for (const auto& [guid, n] : book->data().nodes) {
        lines += n.choices.size();
        if (n.type == "Automatic" && n.choices.empty()) ++empty_automatic;
        for (const auto& c : n.choices) {
            gated += c.requirement.empty() ? 0 : 1;
            if (c.next != 0 && !book->node(c.next)) ++dangling;
        }
    }
    CHECK(lines == 6090 && gated == 1882, "6,090 lines, 1,882 of them with a condition");
    CHECK(dangling == 0, "every line leads to a node of the data (or ends the conversation)");
    size_t bad_roots = 0;
    for (const auto& [root, top] : book->data().roots) bad_roots += (top != 0 && !book->node(top)) ? 1 : 0;
    CHECK(bad_roots == 0, "every root starts at a node of the data");
    CHECK(empty_automatic == 19, "19 automatic nodes have no line (a conversation that reaches one shows nothing)");
    // every condition of every line evaluates, on a character with nothing, without a crash; what it could not answer is counted
    CrierFacts facts;
    std::set<std::string> gaps;
    dl::Evaluator ev(*book, facts, [&](const std::string& s) { gaps.insert(s); });
    size_t holding = 0;
    for (const auto& [guid, n] : book->data().nodes) {
        for (const auto& c : n.choices) holding += ev.holds(c.requirement) ? 1 : 0;
    }
    CHECK(holding >= 4208, "every condition evaluates (the 4,208 lines with none always show)");
    std::printf("  [info] %zu of 6,090 lines show to a character with nothing; %zu kinds of gap (functions or kinds the evaluator does not answer)\n", holding, gaps.size());
    // which NPC opens which root: population_data reads the instances' and assets' roots with the rest of the people
    {
        std::string e;
        auto db = content::ContentDb::open(dialogue_db_path(), e);
        const auto pop = db ? db->population_data(e) : std::nullopt;
        size_t inst = 0, asset = 0;
        bool crier = false;
        if (pop) {
            for (const auto& [g, i] : pop->instances) {
                inst += i.dialogue_root != 0 ? 1 : 0;
                crier = crier || i.dialogue_root == kTownCrierRoot;
            }
            for (const auto& [g, a] : pop->assets) asset += a.dialogue_root != 0 ? 1 : 0;
        }
        CHECK(pop && inst == 1294 && asset == 48 && crier, "1,294 instances and 48 assets open a conversation, the Town Crier's among them");
    }
    // the vendor's node has exactly the three lines retail sent
    const dl::Talk::State* none = nullptr;
    (void)none;
    const auto* n = book->node(6064634349883578023ll);
    CHECK(n && n->choices.size() == 3 && n->type == "Response", "node 6064634349883578023 has exactly 3 lines (the 3 retail sent), indexes 0 to 2");
}

static void test_dialogue_town_crier() {
    std::printf("the Town Crier's lines for a character with no quests\n");
    std::string why;
    const auto book = dialogue_book(why);
    if (!book) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    const auto* greeting = book->node(kTownCrierGreeting);
    size_t plain = 0, gated = 0;
    for (const auto& c : greeting ? greeting->choices : std::vector<content::DialogueChoiceRow>{}) (c.requirement.empty() ? plain : gated)++;
    CHECK(greeting && greeting->choices.size() == 9 && plain == 4 && gated == 5, "9 lines: 4 with no condition and 5 with one");
    const auto* top = book->node(book->top_node(kTownCrierRoot));
    CHECK(top && top->type == "Automatic" && top->choices.size() == 1 && top->choices[0].next == kTownCrierGreeting, "the root starts at an automatic node whose one line leads to those nine");

    const auto visible = [&](CrierFacts facts) {
        dl::Evaluator ev(*book, facts);
        dl::Talk t;
        const dl::Result r = t.begin(*book, ev, 77, kTownCrierRoot);
        CHECK(r.outcome == dl::Outcome::Changed && t.state().shown_node == kTownCrierGreeting && t.state().greeting, "the click shows the greeting, not the automatic node");
        return line_indexes(t);
    };
    // 0 "How can I help around here?": not in Lionhold and not a citizen.  1 construction: none under construction.  2 the election: not a citizen and none.
    // 3 "new construction": a false expression on purpose.  4 "looking for work": its quest needs another quest active.  5 to 8: always.
    CHECK(visible(CrierFacts{}) == std::vector<int32_t>({0, 5, 6, 7, 8}), "a new character in a town: lines 0, 5, 6, 7 and 8");
    CrierFacts in_lionhold;
    in_lionhold.node = kLionhold;
    CHECK(visible(in_lionhold) == std::vector<int32_t>({5, 6, 7, 8}), "in Lionhold the first line is hidden");
    CrierFacts citizen;
    citizen.citizen = true;
    CHECK(visible(citizen) == std::vector<int32_t>({5, 6, 7, 8}), "a citizen does not get the first line, and the election line needs an election");
    // "Exit." ends the conversation; "Show me the bulletin." does nothing yet; "Can you tell me about this settlement?" leads on
    CrierFacts f;
    dl::Evaluator ev(*book, f);
    dl::Talk t;
    t.begin(*book, ev, 77, kTownCrierRoot);
    dl::Result r = t.choose(*book, ev, choice_of(kTownCrierGreeting, 7));
    CHECK(r.outcome == dl::Outcome::Nothing && t.open() && !r.skipped.empty(), "the bulletin line opens a window: skipped, the conversation stays as it is");
    r = t.choose(*book, ev, choice_of(kTownCrierGreeting, 5));
    CHECK(r.outcome == dl::Outcome::Changed && t.state().shown_node == 6064634349883577707ll && !t.state().greeting && t.state().previous_choice_node == kTownCrierGreeting &&
              t.state().previous_choice_index == 5 && t.state().current_node == 6064634349883577707ll && t.state().previous_node == 0,
          "\"about this settlement\" leads on: the greeting is the line's node, the next node the current one");
    CHECK(line_indexes(t) == std::vector<int32_t>({0, 1, 2}), "its three lines show");
    r = t.choose(*book, ev, choice_of(6064634349883577707ll, 1));   // "I have another question." back to the nine
    CHECK(r.outcome == dl::Outcome::Changed && t.state().shown_node == kTownCrierGreeting && t.state().previous_node == 6064634349883577707ll, "\"another question\" goes back, the node left is the previous node");
    r = t.choose(*book, ev, choice_of(kTownCrierGreeting, 8));
    CHECK(r.outcome == dl::Outcome::Changed && !t.open() && t.state().lines.empty(), "\"Exit.\" ends the conversation");
    // hidden and stale lines
    t.begin(*book, ev, 77, kTownCrierRoot);
    CHECK(t.choose(*book, ev, choice_of(kTownCrierGreeting, 4)).outcome == dl::Outcome::Refused && t.open() && t.state().shown_node == kTownCrierGreeting, "a line that is not shown is refused, nothing changes");
    CHECK(t.choose(*book, ev, choice_of(kTownCrierGreeting, 99)).outcome == dl::Outcome::Refused, "an index beyond the node is refused");
    CHECK(t.choose(*book, ev, choice_of(6064634349883577707ll, 0)).outcome == dl::Outcome::Refused, "a line of another node is refused");
    CHECK(t.begin(*book, ev, 78, kTownCrierRoot).outcome == dl::Outcome::Refused && t.state().source == 77, "a second click while the window is open is refused");
}

// ---- the stream -------------------------------------------------------------------------------------------------------------------

struct PcBlock {
    DlgBits             bits;     // the payload of the controller's RepLayout block
    repl::RepState      state;    // read with the repo's block reader
    std::vector<uint16_t> handles;
};

/// The controller's RepLayout blocks among the chains got[from..], in order.
static std::vector<PcBlock> pc_blocks_since(const Harness& h, size_t from) {
    std::vector<PcBlock> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch || h.got[i].open || h.got[i].close) continue;
        const uint8_t* data = h.got[i].content.data();
        const auto blocks = repl::read_content_blocks(data, h.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || !b.header.has_rep_layout) continue;
            size_t pos = b.payload_pos;
            const auto st = repl::read_rep_block(class_named("AoCPlayerControllerBP_C"), data, b.payload_pos + b.payload_bits, pos);
            if (!st) continue;
            PcBlock p;
            for (size_t k = 0; k < b.payload_bits; ++k) {
                const size_t at = b.payload_pos + k;
                p.bits.b.push_back(((data[at >> 3] >> (at & 7)) & 1) != 0);
            }
            p.state = *st;
            for (const auto& [handle, v] : st->values()) {
                if (handle >= 141 && handle <= 170) p.handles.push_back(handle);
            }
            if (!p.handles.empty()) out.push_back(std::move(p));
        }
    }
    return out;
}

/// What the client holds of CurrentDialogueInstance, built the way it builds it: from the blocks, the array's elements merged with the elements it had.
struct ClientDialogue {
    int64_t                    previous_node = 0, previous_choice_node = 0, current_node = 0, shown_node = 0;
    int64_t                    previous_choice_index = 0;
    bool                       greeting = false;
    emit::FIntrepidNetworkGUID source;
    struct Line { int64_t node = 0, type_id = 0, index = 0, kind = 0; };
    std::vector<Line>          lines;
    void apply(const repl::RepState& s) {
        const auto num = [&](uint16_t h, int64_t& into) { if (const repl::Value* v = s.get(h)) if (const int64_t* i = v->get_if<int64_t>()) into = *i; };
        num(143, previous_node); num(146, previous_choice_node); num(149, previous_choice_index); num(151, current_node); num(166, shown_node);
        if (const repl::Value* v = s.get(156)) if (const auto* g = v->get_if<emit::FIntrepidNetworkGUID>()) source = *g;
        if (const repl::Value* v = s.get(170)) if (const bool* b = v->get_if<bool>()) greeting = *b;
        if (const repl::Value* v = s.get(154)) {
            if (const auto* a = v->get_if<repl::ArrayValue>()) {
                lines.resize(a->elements.size());
                for (size_t i = 0; i < a->elements.size(); ++i) {
                    const auto& e = a->elements[i].values;
                    const auto at = [&](uint16_t k) -> int64_t {
                        const auto it = e.find(k);
                        if (it == e.end()) return INT64_MIN;
                        if (const int64_t* x = it->second.get_if<int64_t>()) return *x;
                        if (const uint64_t* u = it->second.get_if<uint64_t>()) return static_cast<int64_t>(*u);
                        return INT64_MIN;
                    };
                    if (at(1) != INT64_MIN) lines[i].node = at(1);
                    if (at(2) != INT64_MIN) lines[i].type_id = at(2);
                    if (at(4) != INT64_MIN) lines[i].index = at(4);
                    if (at(5) != INT64_MIN) lines[i].kind = at(5);
                }
            }
        }
    }
};

struct DialogueHarness : Harness {
    uint64_t talker = 0;
    int64_t  root = 0;
    explicit DialogueHarness(dl::Book book, int64_t root_guid) : Harness(), root(root_guid) {
        fx.world->set_dialogue(std::move(book));
        const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
        if (!vendor_t) { std::printf("  [FAIL] no vendor template\n"); std::exit(2); }
        talker = fx.add(*vendor_t, kCx + 800, kCy + 300, false, 0, "Talker", root);   // 8.5 m
        start();
    }
};

/// The recorded blocks of dialogue_blocks.txt by label.
struct RecordedBlock {
    bool rep = false;
    DlgBits payload;
};
static std::map<std::string, RecordedBlock> recorded_dialogue() {
    std::map<std::string, RecordedBlock> out;
    const std::string path = recordings::find("dialogue_blocks.txt");
    if (path.empty()) return out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string label, dir, hex;
        int rep = 0;
        size_t bits = 0;
        if (!(ss >> label >> dir >> rep >> bits >> hex)) continue;
        if (dir != "S>C") continue;
        out[label] = {rep != 0, DlgBits::of_hex(hex, bits)};
    }
    return out;
}

/// `bits` with the 128-bit object reference `guid` cut out (npos when it is not there): what the recordings are compared by.
static std::optional<DlgBits> without_object(const DlgBits& bits, const emit::FIntrepidNetworkGUID& guid) {
    BunchWriter w;
    emit::write_intrepid_guid(w, guid);
    if (w.bit_pos() != 128) return std::nullopt;
    const DlgBits needle = DlgBits::of_writer(w);
    for (size_t at = 0; at + needle.size() <= bits.size(); ++at) {
        if (bits.slice(at, needle.size()) == needle) {
            DlgBits cut = bits;
            cut.erase(at, needle.size());
            return cut;
        }
    }
    return std::nullopt;
}

static void test_dialogue_session() {
    std::printf("the talking window through the session\n");
    std::string why;
    auto book = dialogue_book(why);
    if (!book) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    constexpr int64_t kGreeting = 6064634349883573705ll, kSecond = 6064634349883573706ll;
    const int64_t root = root_with_top(*book, kGreeting);
    CHECK(root != 0, "a root starts at the node the recorded conversation showed");
    DialogueHarness h(*book, root);
    h.play();
    h.run(milliseconds(1500));
    CHECK(h.s->actor_view()->state_of(h.talker) == world::ClientState::Awake, "the NPC is open on the client");
    const world::WorldActor* npc = h.fx.world->registry().find(h.talker);
    ClientDialogue client;
    const auto wire_ids_exist = [&](const ClientDialogue& c) {
        bool ok = c.shown_node == 0 || book->node(c.shown_node) != nullptr;
        for (const auto& l : c.lines) ok = ok && book->node(l.node) != nullptr && l.type_id == repl::kDialogueNodeTypeId;
        return ok;
    };
    const auto lines_follow_the_rules = [&](const ClientDialogue& c) {
        bool ok = true;
        for (const auto& l : c.lines) {
            const auto* n = book->node(l.node);
            ok = ok && l.node == c.shown_node && n && l.index >= 0 && static_cast<size_t>(l.index) < n->choices.size() && l.kind == 0;
        }
        return ok;
    };

    // the click
    size_t from = h.got.size();
    h.client_request(interact(npc->npc.interact_component), h.pawn_ch);
    auto blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1 && h.s->talk_actor() == h.talker && h.s->dialogue_opens() == 1, "the click is answered with one block and the session talks to the NPC");
    if (blocks.size() == 1) {
        { std::string hs; for (uint16_t x : blocks[0].handles) hs += std::to_string(x) + " "; std::printf("  [info] the greeting block: %zu bits, handles %s\n", blocks[0].bits.size(), hs.c_str()); }
        CHECK(blocks[0].handles == std::vector<uint16_t>({154, 156, 166, 170}), "the greeting carries CurrentChoices, ObjectSource, the node shown and bIsInGreeting: handles 154, 156, 166, 170 (K4)");
        client.apply(blocks[0].state);
        CHECK(wire_ids_exist(client), "K1: the node and the lines name nodes of the data, by the node table's TypeId");
        CHECK(lines_follow_the_rules(client) && client.lines.size() == 1 && client.greeting && client.shown_node == kGreeting, "K2: one line, naming the node shown, index 0, type Default; the greeting flag is on");
        CHECK(client.source == npc->actor.guid, "K3: the source is the NPC the connection has open");
        const auto rec = recorded_dialogue();
        const auto r54 = rec.find("greeting_one_line");
        if (r54 == rec.end()) { recordings::print_skip("dialogue_blocks.txt"); }
        else {
            // the recorded block's own object reference: read it from the block, cut it out of both, compare the rest bit for bit
            size_t pos = 0;
            const std::vector<uint8_t> raw = r54->second.payload.bytes();
            const auto theirs_state = repl::read_rep_block(class_named("AoCPlayerControllerBP_C"), raw.data(), r54->second.payload.size(), pos);
            const repl::Value* ov = theirs_state ? theirs_state->get(156) : nullptr;
            const auto* their_guid = ov ? ov->get_if<emit::FIntrepidNetworkGUID>() : nullptr;
            const auto mine = without_object(blocks[0].bits, npc->actor.guid);
            const auto theirs = their_guid ? without_object(r54->second.payload, *their_guid) : std::nullopt;
            CHECK(mine && theirs && *mine == *theirs && blocks[0].bits.size() == r54->second.payload.size(),
                  "R: the greeting equals the recorded block bit for bit (503 bits), the NPC's object reference apart");
        }
    }
    // a second click: nothing
    from = h.got.size();
    h.client_request(interact(npc->npc.interact_component), h.pawn_ch);
    CHECK(pc_blocks_since(h, from).empty() && h.s->dialogue_opens() == 1, "a second click while the window is open changes nothing");

    // a line that leads on
    from = h.got.size();
    h.client_request(dialogue_choice(choice_of(kGreeting, 0)), h.pc_ch);
    blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1 && h.s->dialogue_steps() == 1, "the line is answered with one block");
    if (blocks.size() == 1) {
        CHECK(blocks[0].handles == std::vector<uint16_t>({146, 151, 154, 166, 170}), "the step carries the line chosen, the node now, its lines, the node shown and the cleared greeting flag: handles 146, 151, 154, 166, 170 (K4)");
        client.apply(blocks[0].state);
        CHECK(client.shown_node == kSecond && client.current_node == kSecond && client.previous_choice_node == kGreeting && !client.greeting && client.lines.size() == 6, "the client shows the second node with its six lines");
        CHECK(wire_ids_exist(client) && lines_follow_the_rules(client), "K1, K2: the ids exist, every line names the node shown with an index below its count");
        const auto rec = recorded_dialogue();
        const auto r94 = rec.find("step_six_lines");
        if (r94 != rec.end()) CHECK(blocks[0].bits == r94->second.payload, "R: the step equals the recorded block bit for bit (1,443 bits)");
    }
    // a line with an action and no way on: nothing (retail opens a confirmation window)
    from = h.got.size();
    h.client_request(dialogue_choice(choice_of(kSecond, 3)), h.pc_ch);
    CHECK(pc_blocks_since(h, from).empty() && h.s->talk().state().shown_node == kSecond, "a line whose action is not done yet changes nothing");

    // hostile calls
    const size_t refused = h.s->dialogue_refused();
    from = h.got.size();
    h.client_request(dialogue_choice(choice_of(kGreeting, 0)), h.pc_ch);
    h.client_request(dialogue_choice(choice_of(kSecond, 17)), h.pc_ch);
    h.client_request(dialogue_choice(choice_of(123456789, 0)), h.pc_ch);
    CHECK(h.s->dialogue_refused() == refused + 3 && pc_blocks_since(h, from).empty() && h.s->talk().state().shown_node == kSecond, "a stale node, an index the node lacks and a node nobody showed are refused: no block, nothing changed");

    // the close: the client sends three calls; the first clears, the others find nothing
    repl::DialogueResponse close;
    close.response_type = 2; close.quest_response = 3; close.node_type_id = repl::kDialogueNodeTypeId;
    repl::DialogueResponse window;
    window.response_type = 3; window.node_type_id = repl::kDialogueNodeTypeId; window.window_type = 4;
    from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, dialogue_choice(close), true), h.client.actor(h.pc_ch, dialogue_choice(close), true), h.client.actor(h.pc_ch, dialogue_choice(window), true)});
    h.settle();
    blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1 && !h.s->talk().open() && h.s->talk_actor() == 0, "the client's three close calls are answered with one block, and the conversation is over");
    if (blocks.size() == 1) {
        CHECK(blocks[0].handles == std::vector<uint16_t>({146, 151, 154, 156, 166}), "the clear carries the line chosen, the node now, no lines, no source and no node shown: handles 146, 151, 154, 156, 166 (K4)");
        client.apply(blocks[0].state);
        CHECK(client.lines.empty() && client.shown_node == 0 && client.current_node == 0 && client.previous_choice_node == 0 && client.source == emit::FIntrepidNetworkGUID{},
              "K3: a close writes the source back to nothing and leaves no node");
        const auto rec = recorded_dialogue();
        const auto r224 = rec.find("clear_after_step");
        if (r224 != rec.end()) CHECK(blocks[0].bits == r224->second.payload, "R: the clear equals the recorded block bit for bit (433 bits)");
    }
    // a choice after the close, and a click again
    from = h.got.size();
    h.client_request(dialogue_choice(choice_of(kSecond, 0)), h.pc_ch);
    CHECK(pc_blocks_since(h, from).empty(), "a choice after the close changes nothing");
    h.client_request(interact(npc->npc.interact_component), h.pawn_ch);
    CHECK(h.s->talk().open() && h.s->dialogue_opens() == 2, "and a new click opens the window again");

    // the NPC goes away mid-talk: the window closes on the server's side
    from = h.got.size();
    h.fx.world->registry().remove(h.talker);
    h.run(milliseconds(300));
    blocks = pc_blocks_since(h, from);
    CHECK(!h.s->talk().open() && !blocks.empty(), "the NPC is gone: the conversation ends and the client is told");
    // the player dies mid-talk
    DialogueHarness d(*book, root);
    d.play();
    d.run(milliseconds(1500));
    const world::WorldActor* npc2 = d.fx.world->registry().find(d.talker);
    d.client_request(interact(npc2->npc.interact_component), d.pawn_ch);
    CHECK(d.s->talk().open(), "a second player talks");
    from = d.got.size();
    d.s->take_damage(d.conn, 1.0e9, d.t);
    d.settle();
    CHECK(!d.s->talk().open() && !pc_blocks_since(d, from).empty(), "the player dies: the conversation ends and the client is told");
    const size_t refused_dead = d.s->dialogue_refused();
    d.client_request(dialogue_choice(choice_of(kGreeting, 0)), d.pc_ch);
    CHECK(d.s->dialogue_refused() == refused_dead + 1, "a choice from a dead player is refused");
}

static void test_dialogue_1531() {
    std::printf("the vendor's greeting as recorded\n");
    std::string why;
    auto book = dialogue_book(why);
    if (!book) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    const auto rec = recorded_dialogue();
    const auto it = rec.find("greeting_three_lines");
    if (it == rec.end()) { recordings::print_skip("dialogue_blocks.txt"); return; }
    const int64_t root = root_with_top(*book, 6064634349883578023ll);
    CHECK(root != 0, "a root starts at the node the recorded vendor showed");
    DialogueHarness h(*book, root);
    h.play();
    h.run(milliseconds(1500));
    const world::WorldActor* npc = h.fx.world->registry().find(h.talker);
    const size_t from = h.got.size();
    h.client_request(interact(npc->npc.interact_component), h.pawn_ch);
    const auto blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1, "the click is answered with one block");
    if (blocks.size() != 1) return;
    size_t pos = 0;
    const std::vector<uint8_t> raw = it->second.payload.bytes();
    const auto theirs_state = repl::read_rep_block(class_named("AoCPlayerControllerBP_C"), raw.data(), it->second.payload.size(), pos);
    const repl::Value* ov = theirs_state ? theirs_state->get(156) : nullptr;
    const auto* their_guid = ov ? ov->get_if<emit::FIntrepidNetworkGUID>() : nullptr;
    const auto mine = without_object(blocks[0].bits, npc->actor.guid);
    const auto theirs = their_guid ? without_object(it->second.payload, *their_guid) : std::nullopt;
    CHECK(mine && theirs && *mine == *theirs && blocks[0].bits.size() == it->second.payload.size(), "R: the vendor's greeting equals the recorded block bit for bit (929 bits), the NPC's object reference apart");
}

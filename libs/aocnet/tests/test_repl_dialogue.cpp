// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_dialogue.cpp
//
//  Quests, slice 0: the talking window's client call (protocol/repl/dialogue_rpc.h) against what the retail client sent.
//
//    * ServerSelectDialogueChoice as recorded (a line chosen, the window closed in the client's three calls: the quest panel's Close twice and the
//      action window's close): each read to its last bit with the schema entry of its RPC, parsed field for field, built again from the typed form and
//      written: the same bits.
//    * The enums and the field set (eight parameters, handles 1 to 8).
//
//  The recorded calls are read from the folder AOC_RECORDINGS_DIR names (dialogue_blocks.txt: one line per recorded content block, "label dir rep bits hex");
//  without them the recorded part prints SKIP.
// ============================================================================
#include "recordings.h"
#include "test_support.h"

#include "protocol/repl/dialogue_rpc.h"
#include "protocol/repl/rpc.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using namespace tsupport;
using emit::BunchWriter;

namespace {

struct Recorded {
    std::string dir;
    bool        rep = false;
    size_t      bits = 0;
    Bits        payload;
};

std::map<std::string, Recorded> load(const std::string& path) {
    std::map<std::string, Recorded> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string label, dir, hex;
        int rep = 0;
        size_t bits = 0;
        if (!(ss >> label >> dir >> rep >> bits >> hex)) continue;
        Recorded r;
        r.dir = dir;
        r.rep = rep != 0;
        r.bits = bits;
        r.payload = bits_of_hex(hex.c_str(), bits);
        out[label] = std::move(r);
    }
    return out;
}

const schema::RpcSchema& select_rpc() {
    const schema::RpcSchema* r = schema_named("AoCPlayerControllerBP_C").rpc("ServerSelectDialogueChoice");
    if (!r) { std::printf("  [FAIL] no ServerSelectDialogueChoice\n"); std::exit(2); }
    return *r;
}

/// The recorded block as the calls it holds.
std::optional<std::vector<RpcCall>> calls_of(const Recorded& r) {
    return with_exact(r.payload, [&](const uint8_t* d, size_t, size_t n) -> std::optional<std::vector<RpcCall>> {
        size_t pos = 0;
        return read_rpc_fields(schema_named("AoCPlayerControllerBP_C"), d, n, pos, {});
    });
}

void check_call(const std::map<std::string, Recorded>& rec, const char* label, const DialogueResponse& want, const std::string& what) {
    const auto it = rec.find(label);
    if (it == rec.end()) { CHECK(false, (what + ": the recording has the block").c_str()); return; }
    const auto calls = calls_of(it->second);
    CHECK(calls && calls->size() >= 1, (what + ": the block reads as RPC fields").c_str());
    if (!calls || calls->empty()) return;
    const RpcCall& c = calls->front();
    CHECK(c.rpc && std::string(c.rpc->name) == "ServerSelectDialogueChoice" && c.decoded, (what + ": it is ServerSelectDialogueChoice and its parameters read to the last bit").c_str());
    CHECK(c.param_bits == 184, (what + ": the parameters are 184 bits").c_str());
    const auto parsed = parse_dialogue_response(c);
    CHECK(parsed.has_value(), (what + ": the typed form reads").c_str());
    if (!parsed) return;
    CHECK(*parsed == want, (what + ": every field is what the player did (type " + std::to_string(want.response_type) + ", quest response " + std::to_string(want.quest_response) +
                            ", node " + std::to_string(want.node) + ", index " + std::to_string(want.choice_index) + ", window " + std::to_string(want.window_type) + ")").c_str());
    // built again from the typed form, written as the field of its class: the recorded bits
    const auto params = dialogue_response_params(select_rpc(), *parsed);
    CHECK(params.has_value(), (what + ": the typed form builds parameters again").c_str());
    if (!params) return;
    BunchWriter w;
    bool failed = false;
    write_rpc_field(schema_named("AoCPlayerControllerBP_C"), select_rpc(), *params, w, {}, &failed);
    CHECK(!failed, (what + ": the writer takes them").c_str());
    // the recorded block holds the field first, then (for the close) the next calls; compare the first field's bits
    const Bits mine(w);
    const Bits theirs = it->second.payload.slice(0, mine.size() <= it->second.payload.size() ? mine.size() : it->second.payload.size());
    CHECK(mine == theirs, (what + ": written again it is the recorded bits (" + std::to_string(mine.size()) + ")").c_str());
}

void test_recorded() {
    std::printf("-- the client's calls as recorded\n");
    const std::string path = recordings::find("dialogue_blocks.txt");
    if (path.empty()) { recordings::print_skip("dialogue_blocks.txt"); return; }
    const auto rec = load(path);
    constexpr int64_t kType = kDialogueNodeTypeId;
    DialogueResponse a;   // the first line of a greeting
    a.response_type = 1; a.node = 6064634349883573705LL; a.node_type_id = kType; a.choice_index = 0;
    check_call(rec, "choice_first_line", a, "line 0 of a greeting");
    DialogueResponse b;   // line 3 of a second node
    b.response_type = 1; b.node = 6064634349883573706LL; b.node_type_id = kType; b.choice_index = 3;
    check_call(rec, "choice_fourth_line", b, "line 3 of a second node");
    DialogueResponse c;   // line 1 of a vendor's greeting
    c.response_type = 1; c.node = 6064634349883578023LL; c.node_type_id = kType; c.choice_index = 1;
    check_call(rec, "choice_second_line", c, "line 1 of a vendor's greeting");
    DialogueResponse close;   // the quest panel's Close, twice
    close.response_type = 2; close.quest_response = 3; close.node_type_id = kType;
    check_call(rec, "close", close, "close, first");
    check_call(rec, "close_2", close, "close, second");
    DialogueResponse window;   // the action window's close (NodeSummary)
    window.response_type = 3; window.node_type_id = kType; window.window_type = 4;
    check_call(rec, "close_3", window, "close, the action window");
}

void test_schema() {
    std::printf("-- the field set\n");
    const schema::RpcSchema& rpc = select_rpc();
    for (const char* p : {"Response.ResponseType", "Response.QuestResponse", "Response.Choice.SourceNodeId.Guid", "Response.Choice.SourceNodeId.TypeId",
                          "Response.Choice.SourceNodeId.Name", "Response.Choice.ChoiceIndex", "Response.Choice.ChoiceType", "Response.WindowType"}) {
        CHECK(rpc_command(rpc, p) != nullptr, (std::string("the RPC has ") + p).c_str());
    }
    // another RPC is not a dialogue response
    RpcCall other;
    CHECK(!parse_dialogue_response(other).has_value(), "a call with no RPC does not parse");
    DialogueResponse r;
    r.response_type = 1; r.node = 5; r.node_type_id = kDialogueNodeTypeId; r.choice_index = 2;
    const auto params = dialogue_response_params(rpc, r);
    CHECK(params.has_value(), "parameters build");
    if (params) {
        RpcCall call;
        call.rpc = &rpc; call.params = *params; call.decoded = true;
        const auto back = parse_dialogue_response(call);
        CHECK(back && *back == r, "built parameters parse back to the same response");
    }
}

}  // namespace

int main() {
    test_schema();
    test_recorded();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

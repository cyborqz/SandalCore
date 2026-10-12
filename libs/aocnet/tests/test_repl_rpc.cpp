// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_rpc.cpp
//
//  protocol/repl/rpc.{h,cpp} and subobject.{h,cpp}'s content blocks against the
//  RPCs in the capture (capture_blocks.h; fixtures/replay_data.bin for the
//  server's, fixtures/e2e/capture_20260928_061401.jsonl for the client's):
//
//    S>C on the PlayerController (ch 3), encoded from parameter values by the
//    class's RpcSchema, wrapped in a content block, bit for bit:
//      ClientSetHUD(BP_AOCHUD_C)            pkt#22 chSeq 957, 173 bits
//      ClientRestart(pawn)                  pkt#97, 173 bits (and wire/possession.h's
//                                           hand-written writer gives the same bits)
//      ClientVoiceHandshakeComplete, ClientFlushLevelStreaming   no parameters
//      ClientEnableNetworkVoice(bool)       a bare bool
//      ClientCapBandwidth(int32)            send bit + value
//      ClientSetTime(double, double)        pkt#23 chSeq 959
//      ClientUpdateMultipleLevelsStreamingStatusCompressed(uint8[429])   3475 bits:
//                                           an array in the plain form
//    C>S, read by read_client_bunch to their last bit: ServerClientIsReadyToPhaseIn,
//    ServerRequestStorageData, ServerSetActiveTarget, ServerNotifyHeldAbilityCancelled
//    (on the AbilityComponent: the client's subobject block) and the 7192-bit bunch
//    of the possession handshake (72 RPCs, ServerAcknowledgePossession among them);
//    each re-encodes to the client's bits.  ServerTriggerAbility, whose ItemData holds the empty
//    FItemInstance, decodes too (plan A7; test_repl_ability_rpc reads all 54 captured calls);
//    RequestHitVerification_Server (an object named by path, ObjectId 1) has no codec
//    here: listed, not decoded.
//    The send bit, defaults, and what write_rpc_params refuses.
//    Hostile client bunches: a RepLayout block is skipped, a stray bit after the parameters
//    is refused, at most 1024 RPCs a bunch.
// ============================================================================
#include "capture_blocks.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/subobject.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/possession.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using capture_blocks::Bits;
// -- C O A x C_Y_B --

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

static FIntrepidNetworkGUID guid(uint64_t id, uint32_t server = 0, uint32_t rnd = 0) {
    FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = server;
    g.Randomizer = rnd;
    return g;
}

/// The actor GUIDs of the captured session: ServerId 60.
static FIntrepidNetworkGUID session_guid(uint64_t id) { return guid(id, 60, 0x6ee87ee4); }

/// True when `w` holds exactly the bits of `b`.
static bool same_as(const BunchWriter& w, const Bits& b) {
    if (w.bit_pos() != b.bits) {
        std::printf("    wrote %zu bits, expected %zu (%s)\n", w.bit_pos(), b.bits, b.name);
        return false;
    }
    const auto want = capture_blocks::bytes_of(b.hex);
    size_t diff = 0;
    if (!capture_blocks::same_bit_range(w.data(), want.data(), b.bits, &diff)) {
        std::printf("    first differing bit %zu (%s)\n", diff, b.name);
        return false;
    }
    return true;
}

/// An S>C RPC on the PlayerController's actor: header, size, field and parameters.
static BunchWriter pc_block(const char* rpc_name, const RpcParams& params, bool* ok = nullptr) {
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc(rpc_name);
    BunchWriter field, block;
    bool failed = rpc == nullptr;
    if (rpc) write_rpc_field(pc, *rpc, params, field, {}, &failed);
    BlockHeader h;   // the channel's actor, no RepLayout block
    if (!failed) write_content_block(block, h, field);
    if (ok) *ok = !failed;
    return block;
}

static Value f64_of(uint64_t bits) { return Value::f64(std::bit_cast<double>(bits)); }

static void test_server_rpcs() {
    std::printf("S>C RPCs on the PlayerController, encoded from values\n");
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    {
        const schema::RpcSchema* rpc = pc.rpc("ClientSetHUD");
        CHECK(rpc && rpc->field_index == 53 && rpc->params.size() == 1 && rpc->params[0].send_bit &&
                  rpc->commands.size() == 1 && rpc->commands[0].handle == 1,
              "ClientSetHUD is field 53 with one object parameter, handle 1, behind a send bit");
    }
    {   // pkt#22 chSeq 957
        RpcParams p;
        const schema::RpcSchema& rpc = *pc.rpc("ClientSetHUD");
        CHECK(set_rpc_param(rpc, p, "NewHUDClass", Value(guid(8463636446363390223ull))), "NewHUDClass = the BP_AOCHUD_C class GUID");
        BunchWriter params;
        CHECK(write_rpc_params(rpc, p, params) == 129, "the parameter is 129 bits: the send bit and the 128-bit GUID");
        bool ok = false;
        const BunchWriter block = pc_block("ClientSetHUD", p, &ok);
        CHECK(ok && same_as(block, capture_blocks::kRpcClientSetHUD), "ClientSetHUD: the 173-bit content block, bit for bit (pkt#22 chSeq 957)");
    }
    {   // pkt#97 chSeq 992, and the hand-written writer of wire/possession.h
        const FIntrepidNetworkGUID pawn = session_guid(10341538);
        RpcParams p;
        set_rpc_param(*pc.rpc("ClientRestart"), p, "NewPawn", Value(pawn));
        const BunchWriter block = pc_block("ClientRestart", p);
        CHECK(same_as(block, capture_blocks::kRpcClientRestart), "ClientRestart(pawn): the 173-bit block, bit for bit (pkt#97)");
        for (const char* name : {"ClientRestart", "ClientRetryClientRestart"}) {
            RpcParams q;
            set_rpc_param(*pc.rpc(name), q, "NewPawn", Value(pawn));
            const BunchWriter mine = pc_block(name, q);
            uint8_t buf[64] = {};
            size_t off = 0;
            const uint32_t field = std::string(name) == "ClientRestart" ? wire::kPcFieldIdx_ClientRestart
                                                                        : wire::kPcFieldIdx_ClientRetryClientRestart;
            const size_t bits = wire::write_pc_object_rpc(buf, sizeof(buf), off, field, pawn);
            size_t diff = 0;
            CHECK(bits == mine.bit_pos() && capture_blocks::same_bit_range(mine.data(), buf, bits, &diff),
                  (std::string(name) + ": the same bits as wire/possession.h's write_pc_object_rpc").c_str());
        }
    }
    {   // pkt#23: no parameters, a bare bool, an int32, two doubles
        CHECK(same_as(pc_block("ClientVoiceHandshakeComplete", {}), capture_blocks::kRpcClientVoiceHandshakeComplete),
              "ClientVoiceHandshakeComplete(): 28 bits (field 70 and a 0-bit payload), bit for bit (pkt#23)");
        CHECK(same_as(pc_block("ClientFlushLevelStreaming", {}), capture_blocks::kRpcClientFlushLevelStreaming),
              "ClientFlushLevelStreaming(): bit for bit (pkt#23)");

        RpcParams off;   // bEnable = false: a bool goes bare, whether set or left out
        set_rpc_param(*pc.rpc("ClientEnableNetworkVoice"), off, "bEnable", Value::boolean(false));
        CHECK(same_as(pc_block("ClientEnableNetworkVoice", off), capture_blocks::kRpcClientEnableNetworkVoice),
              "ClientEnableNetworkVoice(false): 29 bits, a bare bool, bit for bit (pkt#23)");
        CHECK(same_as(pc_block("ClientEnableNetworkVoice", {}), capture_blocks::kRpcClientEnableNetworkVoice),
              "... and the parameter left out is the same bit");
        RpcParams on;
        set_rpc_param(*pc.rpc("ClientEnableNetworkVoice"), on, "bEnable", Value::boolean(true));
        BunchWriter w;
        write_rpc_params(*pc.rpc("ClientEnableNetworkVoice"), on, w);
        CHECK(w.bit_pos() == 1 && (w.data()[0] & 1) == 1, "bEnable = true is one 1 bit");

        RpcParams cap;
        set_rpc_param(*pc.rpc("ClientCapBandwidth"), cap, "Cap", Value::i64(200000));
        CHECK(same_as(pc_block("ClientCapBandwidth", cap), capture_blocks::kRpcClientCapBandwidth),
              "ClientCapBandwidth(200000): send bit + int32, bit for bit (pkt#23)");

        RpcParams time;
        const schema::RpcSchema& st = *pc.rpc("ClientSetTime");
        CHECK(set_rpc_param(st, time, "NewGameTime", f64_of(0x41a4dade8823d1e9ull)) &&
                  set_rpc_param(st, time, "NewSynchronizedTime", f64_of(0x416cba51e07653c4ull)),
              "ClientSetTime's two doubles");
        CHECK(same_as(pc_block("ClientSetTime", time), capture_blocks::kRpcClientSetTime),
              "ClientSetTime(174944068.07, 15061647.01): 174 bits, two send bits and 128, bit for bit (pkt#23)");
    }
    {   // the compressed level statuses: read from the capture, written back
        const Bits& b = capture_blocks::kRpcClientUpdateMultipleLevelsStreamingStatusCompressed;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        size_t pos = 0;
        const auto blocks = read_content_blocks(bytes.data(), b.bits, 0);
        CHECK(blocks && blocks->size() == 1 && !(*blocks)[0].header.has_rep_layout && (*blocks)[0].header.is_actor,
              "the level-status block reads as one content block of the actor, no RepLayout block");
        if (!blocks || blocks->empty()) return;
        pos = (*blocks)[0].payload_pos;
        const auto calls = read_rpc_fields(pc, bytes.data(), pos + (*blocks)[0].payload_bits, pos);
        CHECK(calls && calls->size() == 1 && (*calls)[0].decoded && std::string((*calls)[0].rpc->name) ==
                                                                        "ClientUpdateMultipleLevelsStreamingStatusCompressed",
              "read_rpc_fields: ClientUpdateMultipleLevelsStreamingStatusCompressed, decoded");
        if (!calls || calls->empty()) return;
        const ArrayValue* levels = (*calls)[0].params.values.at(1).get_if<ArrayValue>();
        CHECK(levels && levels->elements.size() == 429 && levels->elements[0].values.at(1) == Value::u64(13) &&
                  levels->elements[4].values.at(1) == Value::u64(165),
              "LevelStatuses is 429 bytes: 13, 2, 0, 0, 165 ...");
        BunchWriter block;
        BunchWriter field;
        bool failed = false;
        write_rpc_field(pc, *(*calls)[0].rpc, (*calls)[0].params, field, {}, &failed);
        BlockHeader h;
        write_content_block(block, h, field);
        CHECK(!failed && same_as(block, b), "written back: 3493 bits of block, an array as its uint16 count and each byte (pkt#23)");
    }
}
// C O A with C:Y:B

static void test_send_bit_and_refusals() {
    std::printf("the send bit, defaults and refusals\n");
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    {   // a zero int32 is the default: only the send bit
        const schema::RpcSchema& rpc = *pc.rpc("ClientCapBandwidth");
        RpcParams zero;
        set_rpc_param(rpc, zero, "Cap", Value::i64(0));
        BunchWriter a, b;
        CHECK(write_rpc_params(rpc, zero, a) == 1 && a.data()[0] == 0, "Cap = 0 is the default: one 0 send bit");
        CHECK(write_rpc_params(rpc, {}, b) == 1 && b.data()[0] == 0, "Cap left out: the same bit");
        size_t pos = 0;
        const auto back = read_rpc_params(rpc, a.data(), 1, pos);
        CHECK(back && pos == 1 && back->values.empty(), "a 0 send bit reads back as the parameter left at its default");
    }
    {   // a null object is the default too
        const schema::RpcSchema& rpc = *pc.rpc("ClientRestart");
        RpcParams p;
        set_rpc_param(rpc, p, "NewPawn", Value(guid(0)));
        BunchWriter w;
        CHECK(write_rpc_params(rpc, p, w) == 1, "a null pawn is one 0 bit (wire/possession.h: send(1)=0 means a null pawn)");
    }
    {   // what does not fit
        const schema::RpcSchema& rpc = *pc.rpc("ClientCapBandwidth");
        RpcParams p;
        CHECK(!set_rpc_param(rpc, p, "Cap", Value::boolean(true)), "an int32 parameter refuses a bool");
        CHECK(!set_rpc_param(rpc, p, "Cap", Value::i64(1ll << 40)), "... and a value beyond 32 bits");
        CHECK(!set_rpc_param(rpc, p, "Nope", Value::i64(1)), "... and a parameter the RPC does not have");
        CHECK(p.values.empty(), "a refused set leaves the params alone");
        p.values[7] = Value::i64(1);   // a handle the RPC does not have
        BunchWriter w;
        bool failed = false;
        CHECK(write_rpc_params(rpc, p, w, {}, &failed) == 0 && failed && w.bit_pos() == 0, "params naming an unknown handle: refused, nothing written");
    }
    {   // a struct with half its members
        const schema::ClassSchema& ability = schema_named("UAoCAbilityComponent");
        const schema::RpcSchema& rpc = *ability.rpc("ServerTriggerAbility");
        RpcParams p;
        set_rpc_param(rpc, p, "ItemData.Slot", Value::i64(3));
        BunchWriter w;
        bool failed = false;
        CHECK(!rpc_params_match(rpc, p) && write_rpc_params(rpc, p, w, {}, &failed) == 0 && failed,
              "ItemData with its Slot but not its ItemInstance: refused (a parameter has all its commands or none)");
    }
    {   // a struct parameter is always sent; an absent one is a 0 bit
        const schema::ClassSchema& ability = schema_named("UAoCAbilityComponent");
        const schema::RpcSchema& rpc = *ability.rpc("RequestHitVerification_Server");   // param HitRequest (struct)
        RpcParams none;
        BunchWriter w;
        CHECK(write_rpc_params(rpc, none, w) == 1, "a struct parameter left out is a 0 send bit");
        const schema::RpcSchema& fv = *ability.rpc("ServerNotifyHeldAbilityCancelled");   // int32, float, int32
        RpcParams p;
        set_rpc_param(fv, p, "CancelledTime", Value::f32(0.0f));
        BunchWriter w2;
        CHECK(write_rpc_params(fv, p, w2) == 3 && w2.data()[0] == 0, "a float 0.0 is default: three 0 send bits for the three parameters");
        RpcParams q;
        set_rpc_param(fv, q, "CancelledTime", Value::f32(0.5f));
        BunchWriter w3;
        CHECK(write_rpc_params(fv, q, w3) == 3 + 32, "a float 0.5 costs 32 more bits");
    }
    {   // an RPC that is not the class's
        const schema::ClassSchema& ability = schema_named("UAoCAbilityComponent");
        const schema::RpcSchema* wrong = pc.rpc("ClientSetHUD");
        BunchWriter w;
        bool failed = false;
        CHECK(write_rpc_field(ability, *wrong, {}, w, {}, &failed) == 0 && failed && w.bit_pos() == 0,
              "write_rpc_field refuses an RPC that belongs to another class");
    }
}

/// The RPC names of a read bunch, in order.
static std::string names_of(const ClientBunch& b) {
    std::string s;
    for (const ClientRpc& r : b.rpcs) s += std::string(s.empty() ? "" : ",") + r.call.rpc->name;
    return s;
}

static const schema::ClassSchema* resolve_ability(const FIntrepidNetworkGUID& g) {
    return g.ObjectId == 10341564 ? &schema_named("UAoCAbilityComponent") : nullptr;   // the captured AbilityComponent
}
// built with Cyb and C-O-A

static void test_client_rpcs() {
    std::printf("the client's RPCs (C>S), read to their last bit\n");
    const schema::ClassSchema& pawn = schema_named("PlayerPawn_C");
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    {
        const Bits& b = capture_blocks::kClientReadyToPhaseIn;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        const auto r = read_client_bunch(pawn, resolve_ability, bytes.data(), b.bits);
        CHECK(r && r->rpcs.size() == 1 && r->skipped_blocks == 0 && names_of(*r) == "ServerClientIsReadyToPhaseIn" &&
                  r->rpcs[0].on_actor && r->rpcs[0].call.decoded && r->rpcs[0].call.param_bits == 0 &&
                  r->rpcs[0].call.rpc->field_index == 77,
              "ServerClientIsReadyToPhaseIn(): pawn field 77, no parameters, the 25-bit bunch reads to its end");
    }
    {
        const Bits& b = capture_blocks::kClientRequestStorageData;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        const auto r = read_client_bunch(pawn, resolve_ability, bytes.data(), b.bits);
        const bool ok = r && r->rpcs.size() == 1 && r->rpcs[0].call.decoded && names_of(*r) == "ServerRequestStorageData";
        CHECK(ok && r->rpcs[0].call.param_bits == 129 &&
                  r->rpcs[0].call.params.values.at(1) == Value(session_guid(10341556)),
              "ServerRequestStorageData(StorageContainer = 10341556): 129 parameter bits");
        if (ok) {   // and back
            BunchWriter field, block;
            bool failed = false;
            write_rpc_field(pawn, *r->rpcs[0].call.rpc, r->rpcs[0].call.params, field, {}, &failed);
            write_content_block(block, BlockHeader{}, field, /*server_form=*/false);
            CHECK(!failed && same_as(block, b), "written back (the client's block form): the 171 bits");
        }
    }
    {
        const Bits& b = capture_blocks::kClientSetActiveTarget;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        const auto r = read_client_bunch(pawn, resolve_ability, bytes.data(), b.bits);
        const bool ok = r && r->rpcs.size() == 1 && r->rpcs[0].call.decoded && names_of(*r) == "ServerSetActiveTarget";
        CHECK(ok && r->rpcs[0].call.params.values.size() == 2 &&
                  r->rpcs[0].call.params.values.at(1) == Value(guid(3814, 60, 0x6ee87ee4)) &&
                  r->rpcs[0].call.params.values.at(2) == Value::u64(1),
              "ServerSetActiveTarget(InActiveTarget = 3814, Slot = 1 in 3 bits)");
        if (ok) {
            BunchWriter field, block;
            write_rpc_field(pawn, *r->rpcs[0].call.rpc, r->rpcs[0].call.params, field);
            write_content_block(block, BlockHeader{}, field, false);
            CHECK(same_as(block, b), "written back: the 175 bits");
        }
    }
    {   // a subobject: the AbilityComponent
        const Bits& b = capture_blocks::kClientHeldAbilityCancelled;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        const auto r = read_client_bunch(pawn, resolve_ability, bytes.data(), b.bits);
        const bool ok = r && r->rpcs.size() == 1 && r->rpcs[0].call.decoded && names_of(*r) == "ServerNotifyHeldAbilityCancelled";
        CHECK(ok && !r->rpcs[0].on_actor && r->rpcs[0].object.ObjectId == 10341564 &&
                  std::string(r->rpcs[0].cls->name) == "UAoCAbilityComponent" && r->rpcs[0].call.param_bits == 67,
              "ServerNotifyHeldAbilityCancelled on subobject 10341564 (UAoCAbilityComponent field 24): 67 parameter bits");
        if (!ok) return;
        const RpcParams& p = r->rpcs[0].call.params;
        const float* t = p.values.at(2).get_if<float>();
        CHECK(p.values.count(1) == 0 && t && *t > 0.2131f && *t < 0.2132f && p.values.at(3) == Value::i64(-1),
              "ClientInstanceId left at its default, CancelledTime 0.21315, CancelSectionIndex -1");
        BunchWriter field, block;
        bool failed = false;
        write_rpc_field(*r->rpcs[0].cls, *r->rpcs[0].call.rpc, p, field, {}, &failed);
        BlockHeader h;
        h.is_actor = false;
        h.object = r->rpcs[0].object;
        write_content_block(block, h, field, false);
        CHECK(!failed && same_as(block, b), "written back: the client's subobject block, the 218 bits");
    }
    {   // the possession handshake's 7192-bit bunch
        const Bits& b = capture_blocks::kClientBunchPossession;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        const auto r = read_client_bunch(pc, resolve_ability, bytes.data(), b.bits);
        CHECK(r && r->skipped_blocks == 0, "the bunch reads block by block to its 7192nd bit");
        if (!r) return;
        size_t node = 0, siege = 0, decoded = 0;
        for (const ClientRpc& c : r->rpcs) {
            node += std::string(c.call.rpc->name) == "ServerRequestNodeSummary";
            siege += std::string(c.call.rpc->name) == "ServerRequestCurrentSiegeSummary";
            decoded += c.call.decoded;
        }
        CHECK(r->rpcs.size() == 72 && node == 34 && siege == 34 && decoded == 72,
              "72 RPCs (ServerUpdateAccountCombatSettings, NotifyAntiCheatState, ServerAcknowledgePossession, ServerRequestClientLoad, 34 x ServerRequestNodeSummary and 34 x ServerRequestCurrentSiegeSummary), every one decoded to its last bit");
        {   // and each one, written back from its values, is its own block of the bunch, bit for bit
            const auto views = read_content_blocks(bytes.data(), b.bits, 0, false);
            size_t same = 0;
            for (size_t i = 0; views && i < views->size() && i < r->rpcs.size(); ++i) {
                const BlockView& v = (*views)[i];
                BunchWriter field, block, cut;
                write_rpc_field(pc, *r->rpcs[i].call.rpc, r->rpcs[i].call.params, field);
                write_content_block(block, BlockHeader{}, field, false);
                cut.write_bit_range(bytes.data(), v.start, v.payload_pos + v.payload_bits - v.start);
                same += block.bit_pos() == cut.bit_pos() && capture_blocks::same_bit_range(block.data(), cut.data(), cut.bit_pos(), nullptr);
            }
            CHECK(views && views->size() == 72 && same == 72, "all 72 blocks rebuilt from the decoded values are the client's bits, bit for bit");
        }
        const ClientRpc* ack = nullptr;
        for (const ClientRpc& c : r->rpcs) {
            if (std::string(c.call.rpc->name) == "ServerAcknowledgePossession") ack = &c;
        }
        CHECK(ack && ack->call.decoded && ack->call.rpc->field_index == 73 && ack->call.param_bits == 129 &&
                  ack->call.params.values.at(1) == Value(session_guid(10341538)),
              "ServerAcknowledgePossession(pawn 10341538): field 73, 129 parameter bits");
        if (ack) {
            // Its block is 173 bits, the client's copy of test_possession's shape: rebuild it and find it in the bunch.
            BunchWriter field, block;
            write_rpc_field(pc, *ack->call.rpc, ack->call.params, field);
            write_content_block(block, BlockHeader{}, field, false);
            const auto blocks = read_content_blocks(bytes.data(), b.bits, 0, false);
            bool found = false;
            for (const BlockView& v : blocks.value_or(std::vector<BlockView>{})) {
                if (v.payload_pos + v.payload_bits - v.start != block.bit_pos()) continue;
                BunchWriter cut;
                cut.write_bit_range(bytes.data(), v.start, block.bit_pos());
                found = found || capture_blocks::same_bit_range(cut.data(), block.data(), block.bit_pos(), nullptr);
            }
            CHECK(block.bit_pos() == 173 && found, "the same 173-bit block, rebuilt from the values, is in the bunch bit for bit");
        }
    }
    {   // ServerTriggerAbility decodes (the empty FItemInstance); RequestHitVerification_Server has no codec: listed, not decoded
        const Bits& b = capture_blocks::kClientTriggerAbility;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        const auto r = read_client_bunch(pawn, resolve_ability, bytes.data(), b.bits);
        CHECK(r && r->rpcs.size() == 1 && names_of(*r) == "ServerTriggerAbility" && r->rpcs[0].call.decoded &&
                  r->rpcs[0].call.param_bits == 238 && r->rpcs[0].call.rpc->field_index == 28,
              "ServerTriggerAbility (ItemData holds the empty FItemInstance): field 28, 238 parameter bits, decoded");
        const Bits& h = capture_blocks::kClientHitVerification;
        const auto hb = capture_blocks::bytes_of(h.hex);
        const auto hr = read_client_bunch(pawn, resolve_ability, hb.data(), h.bits);
        CHECK(hr && hr->rpcs.size() == 1 && names_of(*hr) == "RequestHitVerification_Server" && !hr->rpcs[0].call.decoded &&
                  hr->rpcs[0].call.param_bits == 815,
              "RequestHitVerification_Server (a component named by path, ObjectId 1): 815 parameter bits, listed but not decoded");
    }
    {   // a subobject the resolver does not know: its block is skipped
        const Bits& b = capture_blocks::kClientHeldAbilityCancelled;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        const auto r = read_client_bunch(pawn, nullptr, bytes.data(), b.bits);
        CHECK(r && r->rpcs.empty() && r->skipped_blocks == 1, "an unresolved subobject's block is skipped whole");
        std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + 10);   // truncated in the middle of the block
        CHECK(!read_client_bunch(pawn, resolve_ability, cut.data(), 80), "a block that runs past the bunch is malformed");
    }
}
// Cyb - C-O-A

static void test_blocks() {
    std::printf("content block headers\n");
    // The server's form, and its parts: the empty block of pkt#78 (139 bits), the dynamic form of pkt#22 and #107.
    {
        BlockHeader h;
        h.is_actor = false;
        h.object = session_guid(10341544);
        BunchWriter out;
        CHECK(write_content_block(out, h, BunchWriter{}) == 139, "an empty stably-named subobject block is 139 bits: 2 + 128 + 1 + 8");
        const auto v = read_content_blocks(out.data(), out.bit_pos(), 0);
        CHECK(v && v->size() == 1 && (*v)[0].header == h && (*v)[0].payload_bits == 0, "and reads back");
    }
    {
        BlockHeader h;
        h.has_rep_layout = true;
        h.is_actor = false;
        h.object = session_guid(10341556);
        h.stably_named = false;
        h.dynamic_class = guid(11144499416456600109ull);
        BunchWriter payload;
        payload.write_bits(0x2a5, 22);
        BunchWriter out;
        const size_t bits = write_content_block(out, h, payload);
        CHECK(bits == 2 + 128 + 1 + 1 + 128 + 1 + 8 + 22, "a dynamic subobject block: object, not stable, not destroyed, class, actor is outer");
        const auto v = read_content_blocks(out.data(), out.bit_pos(), 0);
        CHECK(v && v->size() == 1 && (*v)[0].header == h && (*v)[0].payload_bits == 22, "and reads back");
        h.outer = session_guid(10341600);
        BunchWriter out2;
        CHECK(write_content_block(out2, h, payload) == bits + 128, "with an outer other than the actor: its GUID follows");
        const auto v2 = read_content_blocks(out2.data(), out2.bit_pos(), 0);
        CHECK(v2 && (*v2)[0].header == h, "and reads back");
    }
    {
        BlockHeader h;
        h.is_actor = false;
        h.object = guid(1);   // the export-flags form of a reference
        BunchWriter out;
        CHECK(write_content_block(out, h, BunchWriter{}) == 0 && out.bit_pos() == 0, "a block naming ObjectId 1 is refused, nothing written");
    }
    {   // the capture's own blocks: the PC open, the pawn open
        const Bits& b = capture_blocks::kPcOpenContent;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        // SerializeNewActor takes 468 bits and the PlayerController's fields 40 (test_serialize_new_actor).
        const auto v = read_content_blocks(bytes.data(), b.bits, 508);
        CHECK(v && v->size() == 2 && (*v)[0].header.is_actor && (*v)[0].header.has_rep_layout && (*v)[0].payload_bits == 519 &&
                  !(*v)[1].header.is_actor && !(*v)[1].header.stably_named && !(*v)[1].header.outer &&
                  (*v)[1].header.dynamic_class == guid(485698175673737329ull) && (*v)[1].header.object == session_guid(10341532) &&
                  (*v)[1].payload_bits == 0,
              "the PC open: the actor's 519-bit block, then GlobalGMCommands 10341532 as a dynamic subobject, 0 bits");
    }
}
// C.O.A : Cyb

// ---- hostile client bunches ---------------------------------------------------------------------------------------------------

// ---- T4: a client's property block is skipped, not read as RPC fields ------------------------------------------------------------
static void t4_property_block_skipped() {
    std::printf("a client bunch with a RepLayout block and an RPC\n");
    const schema::ClassSchema& pawn = schema_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerClientIsReadyToPhaseIn");
    BunchWriter rpc_payload;
    bool failed = false;
    const bool have = rpc && write_rpc_field(pawn, *rpc, RpcParams{}, rpc_payload, {}, &failed) > 0 && !failed;
    BunchWriter junk;   // 40 bits that do not parse as a field loop
    for (int i = 0; i < 40; ++i) junk.write_bit(1);
    BunchWriter bunch;
    BlockHeader props;
    props.has_rep_layout = true;
    const bool ok = have && write_content_block(bunch, props, junk, false) > 0 && write_content_block(bunch, BlockHeader{}, rpc_payload, false) > 0;
    const auto r = ok ? read_client_bunch(pawn, nullptr, bunch.data(), bunch.bit_pos()) : std::nullopt;
    CHECK(r && r->skipped_blocks == 1 && r->rpcs.size() == 1 && std::string(r->rpcs[0].call.rpc->name) == "ServerClientIsReadyToPhaseIn",
          "the RepLayout block is skipped and the RPC after it is read");
}

// ---- T8: a stray bit after an RPC's parameters -----------------------------------------------------------------------------------------
static void t8_trailing_bits() {
    std::printf("an RPC payload with a bit left over\n");
    const schema::ClassSchema& pawn = schema_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerClientIsReadyToPhaseIn");
    BunchWriter w;
    w.write_bit(1);
    size_t pos = 0;
    CHECK(rpc && !read_rpc_params(*rpc, w.data(), 1, pos).has_value() && pos == 0, "read_rpc_params refuses the leftover bit");
}

// ---- the number of RPCs in one client bunch ------------------------------------------------------------------------------------------
static void test_rpc_cap() {
    std::printf("at most %zu RPCs in a client bunch\n", kMaxClientRpcsPerBunch);
    const schema::ClassSchema& pawn = schema_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerClientIsReadyToPhaseIn");
    BunchWriter field;
    bool failed = false;
    if (!rpc || write_rpc_field(pawn, *rpc, RpcParams{}, field, {}, &failed) == 0 || failed) {
        CHECK(false, "ServerClientIsReadyToPhaseIn's field");
        return;
    }
    // n RPC blocks of their own (the client's form: two bits and the field)
    auto blocks = [&](size_t n) {
        BunchWriter b;
        for (size_t i = 0; i < n; ++i) write_content_block(b, BlockHeader{}, field, false);
        return b;
    };
    // n fields in a field loop, and in one block
    auto raw_fields = [&](size_t n) {
        BunchWriter payload;
        for (size_t i = 0; i < n; ++i) payload.write_bit_range(field.data(), 0, field.bit_pos());
        return payload;
    };
    auto fields = [&](size_t n) {
        BunchWriter b;
        write_content_block(b, BlockHeader{}, raw_fields(n), false);
        return b;
    };
    const BunchWriter ok = blocks(kMaxClientRpcsPerBunch);
    const auto r = read_client_bunch(pawn, nullptr, ok.data(), ok.bit_pos());
    CHECK(r && r->rpcs.size() == kMaxClientRpcsPerBunch, "a bunch of exactly the maximum reads");
    const BunchWriter over = blocks(kMaxClientRpcsPerBunch + 1);
    CHECK(!read_client_bunch(pawn, nullptr, over.data(), over.bit_pos()).has_value(), "one RPC more: refused");
    const BunchWriter packed = fields(kMaxClientRpcsPerBunch);
    const auto rp = read_client_bunch(pawn, nullptr, packed.data(), packed.bit_pos());
    CHECK(rp && rp->rpcs.size() == kMaxClientRpcsPerBunch, "the maximum in one block reads");
    const BunchWriter packed_over = fields(kMaxClientRpcsPerBunch + 1);
    CHECK(!read_client_bunch(pawn, nullptr, packed_over.data(), packed_over.bit_pos()).has_value(), "and one more in a block: refused");
    const BunchWriter loop = raw_fields(kMaxClientRpcsPerBunch + 1);
    size_t pos = 0;
    CHECK(!read_rpc_fields(pawn, loop.data(), loop.bit_pos(), pos).has_value() && pos == 0, "read_rpc_fields itself refuses a field loop of more");
    // halves across blocks add up
    BunchWriter halves = blocks(kMaxClientRpcsPerBunch / 2 + 1);
    const BunchWriter more = blocks(kMaxClientRpcsPerBunch / 2);
    halves.write_bit_range(more.data(), 0, more.bit_pos());
    CHECK(!read_client_bunch(pawn, nullptr, halves.data(), halves.bit_pos()).has_value(), "the count is the bunch's, over all its blocks");
}

int main() {
    test_server_rpcs();
    test_send_bit_and_refusals();
    test_client_rpcs();
    test_blocks();
    t4_property_block_skipped();
    t8_trailing_bits();
    test_rpc_cap();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

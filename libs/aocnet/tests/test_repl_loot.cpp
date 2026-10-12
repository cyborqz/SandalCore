// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_loot.cpp
//
//  Plan E1, stage 3, part 2: the loot window's RPCs (protocol/repl/storage_rpc.h) against what the retail server and client sent when a corpse's loot was
//  taken (tests/capture_loot.h: captures 551, 552, 553 and 517).
//
//    * every captured call is read by read_rpc_params with the PlayerController's schema entry and written back to the same bits;
//    * the loot payloads (ClientReceiveLootStoragePayload, four of them) parse into the typed form (LootActor, slots, LootAllocationMethods) and the typed
//      form builds the same parameters again;
//    * the client's ServerRequestLootStorageEvents parses into its events (type 3, the loot container's slot, the bag's target) ;
//    * ClientRemoveListenerFromStorage is built from the corpse's guid alone.
// ============================================================================
#include "capture_loot.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/schema/generated/index.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
namespace gen = aoc::protocol::schema::gen;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
// -- C | Y | B & CoA --

static std::vector<uint8_t> bytes_of_hex(const char* hex) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
    return out;
}

static bool same_bits(const BunchWriter& w, const std::vector<uint8_t>& want, size_t bits) {
    if (w.bit_pos() != bits) return false;
    const std::vector<uint8_t> got = w.bytes();
    for (size_t i = 0; i < bits; ++i) {
        if (((got[i >> 3] >> (i & 7)) & 1) != ((want[i >> 3] >> (i & 7)) & 1)) return false;
    }
    return true;
}

int main() {
    const schema::ClassSchema* pc = gen::find_class_schema("AoCPlayerControllerBP_C");
    if (!pc) {
        std::printf("  [FAIL] no PlayerController schema\n");
        return 2;
    }
    std::printf("the loot window's RPCs against the retail captures\n");
    CodecOptions opts;
    opts.inline_objects = true;
    size_t payloads = 0, requests = 0, listeners = 0, events = 0;
    for (const capture_loot::Rpc& c : capture_loot::kRpcs) {
        const std::string label = std::string(c.rpc) + " (capture " + std::to_string(c.capture) + ", line " + std::to_string(c.line) + ")";
        const schema::RpcSchema* rpc = pc->rpc(c.rpc);
        CHECK(rpc != nullptr, (label + ": the PlayerController has the RPC").c_str());
        if (!rpc) continue;
        const std::vector<uint8_t> data = bytes_of_hex(c.hex);
        size_t pos = 0;
        auto params = read_rpc_params(*rpc, data.data(), c.bits, pos, opts);
        CHECK(params.has_value() && pos == c.bits, (label + ": read to the last bit").c_str());
        if (!params || pos != c.bits) continue;
        BunchWriter w;
        bool failed = false;
        write_rpc_params(*rpc, *params, w, opts, &failed);
        CHECK(!failed && same_bits(w, data, c.bits), (label + ": written back to the same bits").c_str());

        RpcCall call;
        call.rpc = rpc;
        call.params = *params;
        call.decoded = true;
        const std::string name = c.rpc;
        if (name == "ClientReceiveLootStoragePayload") {
            ++payloads;
            const auto p = parse_loot_payload(call);
            CHECK(p.has_value(), (label + ": parses").c_str());
            if (!p) continue;
            CHECK(p->allocation.default_method == 1 && p->allocation.by_rarity[0] == 0 && p->allocation.by_rarity[6] == 0, (label + ": LootAllocationMethods {Default 1, the rest 0}").c_str());
            CHECK(p->data.client_retrieve_only && p->data.maximum_items == static_cast<int32_t>(p->data.storage.size()) && filled_slots(p->data) == p->data.storage.size(),
                  (label + ": retrieve only, as many slots as items").c_str());
            bool permitted = true;
            for (const StorageSlot& s : p->data.storage) permitted = permitted && s.check_loot_permission && s.loot_permission.size() == 1;
            CHECK(permitted, (label + ": every slot checks the loot permission of one player").c_str());
            const auto back = loot_payload_params(*rpc, *p);
            CHECK(back.has_value() && back->values == params->values, (label + ": the typed payload builds the same parameters").c_str());
        } else if (name == "ServerRequestLootStorageEvents") {
            ++requests;
            const auto ev = parse_loot_events(call);
            CHECK(ev.has_value() && ev->size() == 2, (label + ": two events").c_str());
            if (!ev || ev->size() != 2) continue;
            bool shape = true;
            for (const ItemStorageEvent& e : *ev) {
                shape = shape && e.type == 3 && e.move_source.slots.size() == 1 && e.targets.size() == 1 && e.targets[0].slots.size() == 1 && e.item.has_item &&
                        e.move_source.slots[0].item_guid == e.item.guid;
            }
            CHECK(shape, (label + ": each is a type-3 move of one slot of the loot container into one slot of the bag").c_str());
            CHECK((*ev)[0].move_source.slots[0].slot == 0 && (*ev)[1].move_source.slots[0].slot == 1, (label + ": the slots are the window's").c_str());
        } else if (name == "ClientReceiveStorageEvent") {
            ++events;
            const auto e = parse_storage_event(call);
            CHECK(e.has_value() && e->type == 1 && e->targets.size() == 1 && e->targets[0].status == 14, (label + ": a type-1 event naming the loot container").c_str());
        } else if (name == "ClientRemoveListenerFromStorage") {
            ++listeners;
            const auto it = params->values.find(rpc_command(*rpc, "LootActor")->handle);
            const auto* actor = it == params->values.end() ? nullptr : it->second.get_if<emit::FIntrepidNetworkGUID>();
            CHECK(actor != nullptr, (label + ": names the corpse").c_str());
            if (!actor) continue;
            const auto back = remove_listener_params(*rpc, *actor);
            CHECK(back.has_value() && back->values == params->values, (label + ": built from the corpse's guid alone").c_str());
        }
    }
    CHECK(payloads == 4 && requests == 1 && listeners == 1 && events == 1, "the captured calls were all checked");
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

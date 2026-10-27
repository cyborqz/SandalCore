// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_warehouse.cpp
//
//  Storage clerks, step 0: the warehouse window's RPCs (protocol/repl/warehouse_rpc.h, warehouse_codec.h) against what the retail server and client sent.
//
//    * ClientOpenWarehouseWindow and ClientReceiveWarehouseSummary as recorded: each read to its last bit with the schema entry of its RPC, parsed field for field, built again
//      from the typed form and written: the same bits.  The summaries are a town of the fourth level (9 item chunks, 10 material tabs, 1 cargo tab) and a village's.
//    * The client's three requests (the summary, the fetch, the visit).
//    * The storage payloads of an empty material tab (128 slots, an 8 x 16 grid), an empty item chunk (8 slots, a list) and an empty cargo tab (48 slots, 6 x 8): header and slots.
//    * The generated schemas of the two container classes and the owner's lists that name them.
//
//  The recorded blocks are read from the folder AOC_RECORDINGS_DIR names (warehouse_blocks.txt: one line per recorded content block, "label dir rep bits hex"); without them the
//  recorded part prints SKIP.
// ============================================================================
#include "recordings.h"
#include "test_support.h"

#include "protocol/repl/rpc.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/warehouse_rpc.h"
#include "protocol/schema/generated/index.h"

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

const schema::RpcSchema& rpc_of(const char* cls, const char* name) {
    const schema::RpcSchema* r = schema_named(cls).rpc(name);
    if (!r) { std::printf("  [FAIL] no %s on %s\n", name, cls); std::exit(2); }
    return *r;
}

/// The recorded block as the calls it holds.
std::optional<std::vector<RpcCall>> calls_of(const Recorded& r, const char* cls) {
    return with_exact(r.payload, [&](const uint8_t* d, size_t, size_t n) -> std::optional<std::vector<RpcCall>> {
        size_t pos = 0;
        return read_rpc_fields(schema_named(cls), d, n, pos, {});
    });
}

/// The first call of the recorded block, which must be `name` with its parameters read to the last bit.
const RpcCall* first_call(const std::vector<RpcCall>& calls, const char* name, const std::string& what) {
    CHECK(!calls.empty(), (what + ": the block reads as RPC fields").c_str());
    if (calls.empty()) return nullptr;
    const RpcCall& c = calls.front();
    CHECK(c.rpc && std::string(c.rpc->name) == name && c.decoded, (what + ": it is " + name + " and its parameters read to the last bit").c_str());
    return c.rpc && std::string(c.rpc->name) == name && c.decoded ? &c : nullptr;
}

/// The field written again from `params`, compared with the recorded block's first bits.
void check_bits(const Recorded& rec, const char* cls, const schema::RpcSchema& rpc, const RpcParams& params, const std::string& what) {
    BunchWriter w;
    bool failed = false;
    write_rpc_field(schema_named(cls), rpc, params, w, {}, &failed);
    CHECK(!failed, (what + ": the writer takes the parameters").c_str());
    const Bits mine(w);
    const Bits theirs = rec.payload.slice(0, mine.size() <= rec.payload.size() ? mine.size() : rec.payload.size());
    CHECK(mine == theirs, (what + ": written again it is the recorded bits (" + std::to_string(mine.size()) + ")").c_str());
}

constexpr int64_t kGold = 6064628901272551424LL;          // the currency the costs are in
constexpr int64_t kCurrencyType = 223879564105432988LL;   // its record type id

void check_summary(const std::map<std::string, Recorded>& rec, const char* label, int64_t node, const std::vector<bool>& chunks_available, const std::vector<bool>& tabs_available) {
    const std::string what = label;
    const auto it = rec.find(label);
    if (it == rec.end()) { CHECK(false, (what + ": the recording has the block").c_str()); return; }
    const auto calls = calls_of(it->second, "AoCPlayerControllerBP_C");
    if (!calls) { CHECK(false, (what + ": the block reads").c_str()); return; }
    const RpcCall* c = first_call(*calls, "ClientReceiveWarehouseSummary", what);
    if (!c) return;
    CHECK(c->param_bits == 6068, (what + ": the parameters are 6,068 bits").c_str());
    const auto s = parse_warehouse_summary(*c);
    CHECK(s.has_value(), (what + ": the typed form reads").c_str());
    if (!s) return;
    CHECK(s->id.area == kWarehouseAreaNode && s->id.record == node, (what + ": the warehouse is the node's").c_str());
    CHECK(s->resource_def == 6064631794324733952LL && s->crate_def == 6064632025620086784LL, (what + ": the two defs are the default material tabs' and the default cargo tab's").c_str());
    CHECK(s->chunks.size() == 9 && s->tabs.size() == 10 && s->crates.size() == 1, (what + ": 9 item chunks, 10 material tabs and 1 cargo tab").c_str());
    if (s->chunks.size() == 9 && s->tabs.size() == 10 && s->crates.size() == 1) {
        // the free ones list no cost; every other one lists one cost, in gold, of the amount the data names
        CHECK(s->chunks[0].record == 6064631806178426880LL && s->chunks[0].available && s->chunks[0].costs.empty(), (what + ": the base item chunk is free and available").c_str());
        CHECK(s->tabs[0].record == 6064631957531525120LL && s->tabs[0].available && s->tabs[0].costs.empty(), (what + ": the base material tab is free and available").c_str());
        CHECK(s->crates[0].record == 6064631963946647553LL && s->crates[0].available && s->crates[0].costs.empty(), (what + ": the cargo tab is free and available").c_str());
        const double chunk_gold[9] = {0, 1, 5, 5, 10, 10, 15, 15, 20};
        const double tab_gold[10] = {0, 0, 1, 5, 5, 10, 10, 15, 15, 20};
        bool costs_ok = true;
        for (size_t i = 1; i < 9; ++i) {
            costs_ok = costs_ok && s->chunks[i].costs.size() == 1 && s->chunks[i].costs[0].currency == kGold && s->chunks[i].costs[0].type_id == kCurrencyType &&
                       s->chunks[i].costs[0].value == chunk_gold[i];
        }
        for (size_t i = 1; i < 10; ++i) {
            costs_ok = costs_ok && s->tabs[i].costs.size() == 1 && s->tabs[i].costs[0].currency == kGold && s->tabs[i].costs[0].type_id == kCurrencyType &&
                       s->tabs[i].costs[0].value == tab_gold[i];
        }
        CHECK(costs_ok, (what + ": the costs are the data's (chunks 1 5 5 10 10 15 15 20 gold, tabs 0 1 5 5 10 10 15 15 20)").c_str());
        bool avail_ok = true;
        for (size_t i = 0; i < 9; ++i) avail_ok = avail_ok && s->chunks[i].available == chunks_available[i];
        for (size_t i = 0; i < 10; ++i) avail_ok = avail_ok && s->tabs[i].available == tabs_available[i];
        CHECK(avail_ok, (what + ": availability is what the character's standing gives").c_str());
    }
    // built again from the typed form: the recorded bits
    const auto params = warehouse_summary_params(rpc_of("AoCPlayerControllerBP_C", "ClientReceiveWarehouseSummary"), *s);
    CHECK(params.has_value(), (what + ": the typed form builds parameters again").c_str());
    if (params) check_bits(it->second, "AoCPlayerControllerBP_C", rpc_of("AoCPlayerControllerBP_C", "ClientReceiveWarehouseSummary"), *params, what);
}

void check_payload(const std::map<std::string, Recorded>& rec, const char* label, int slots, double columns, double rows) {
    const std::string what = label;
    const auto it = rec.find(label);
    if (it == rec.end()) { CHECK(false, (what + ": the recording has the block").c_str()); return; }
    const auto calls = calls_of(it->second, "PlayerPawn_C");
    if (!calls) { CHECK(false, (what + ": the block reads").c_str()); return; }
    const RpcCall* c = first_call(*calls, "ClientReceiveStoragePayload", what);
    if (!c) return;
    const auto p = parse_storage_payload(*c);
    CHECK(p.has_value(), (what + ": the typed form reads").c_str());
    if (!p) return;
    CHECK(p->maximum_items == slots && static_cast<int>(p->storage.size()) == slots, (what + ": " + std::to_string(slots) + " slots").c_str());
    CHECK(p->dimension.x == columns && p->dimension.y == rows, (what + ": the grid is " + std::to_string(static_cast<int>(columns)) + " x " + std::to_string(static_cast<int>(rows))).c_str());
    CHECK(filled_slots(*p) == 0 && p->override_matrix.empty() && p->container_name.empty() && !p->client_retrieve_only, (what + ": nothing lies in it, no matrix, no name, not read-only").c_str());
    CHECK(p->maximum_weight == 0 && !p->loot_fountain_on_death && !p->randomize_angular, (what + ": the weights, impulses and loot flags are zero").c_str());
    const auto params = storage_payload_params(rpc_of("PlayerPawn_C", "ClientReceiveStoragePayload"), *p);
    CHECK(params.has_value(), (what + ": the typed form builds parameters again").c_str());
    if (params) check_bits(it->second, "PlayerPawn_C", rpc_of("PlayerPawn_C", "ClientReceiveStoragePayload"), *params, what);
}

void test_recorded() {
    std::printf("-- the window's calls as recorded\n");
    const std::string path = recordings::find("warehouse_blocks.txt");
    if (path.empty()) { recordings::print_skip("warehouse_blocks.txt"); return; }
    const auto rec = load(path);

    // ClientOpenWarehouseWindow(WarehouseId{Node, Joeva}, 7): 75 bits
    if (const auto it = rec.find("open_window"); it != rec.end()) {
        const auto calls = calls_of(it->second, "AoCPlayerControllerBP_C");
        const RpcCall* c = calls ? first_call(*calls, "ClientOpenWarehouseWindow", "the window") : nullptr;
        if (c) {
            CHECK(c->param_bits == 75, "the window: the parameters are 75 bits");
            const auto o = parse_open_warehouse_window(*c);
            CHECK(o && o->id.area == kWarehouseAreaNode && o->id.record == 108646121231993LL && o->access_config == kAccessClerk, "the window: the node's warehouse, the clerk's access config 7");
            if (o) {
                const auto params = open_warehouse_window_params(rpc_of("AoCPlayerControllerBP_C", "ClientOpenWarehouseWindow"), o->id, o->access_config);
                CHECK(params.has_value(), "the window: the typed form builds parameters again");
                if (params) check_bits(it->second, "AoCPlayerControllerBP_C", rpc_of("AoCPlayerControllerBP_C", "ClientOpenWarehouseWindow"), *params, "the window");
            }
        }
    } else {
        CHECK(false, "the recording has the window block");
    }

    // the summaries: Joeva (a town of the fourth level, the character no citizen: General rows of levels 3 and 4 may be bought), Kal Torhum's village
    check_summary(rec, "summary_town", 108646121231993LL, {true, false, true, false, true, false, false, false, false}, {true, false, false, true, false, true, false, false, false, false});
    // (Kal Torhum's character holds a rank there that opens the citizen rows of levels 3 and 4 too)
    check_summary(rec, "summary_kal_torhum", 6064633652044300331LL, {true, true, true, true, true, false, false, false, false}, {true, true, true, true, true, true, false, false, false, false});

    // the client's requests
    if (const auto it = rec.find("request_summary"); it != rec.end()) {
        const auto calls = calls_of(it->second, "AoCPlayerControllerBP_C");
        const RpcCall* c = calls ? first_call(*calls, "ServerRequestWarehouseSummary", "the summary request") : nullptr;
        if (c) {
            CHECK(c->param_bits == 68, "the summary request: 68 bits");
            const auto q = parse_warehouse_request(*c);
            CHECK(q && q->id.area == kWarehouseAreaNode && q->id.record == 108646121231993LL, "the summary request names the node");
        }
    }
    if (const auto it = rec.find("request_fetch_general"); it != rec.end()) {
        const auto calls = calls_of(it->second, "PlayerPawn_C");
        const RpcCall* c = calls ? first_call(*calls, "ServerFetchWarehouse", "the fetch") : nullptr;
        if (c) {
            CHECK(c->param_bits == 76, "the fetch: 76 bits (the storage type left at its default)");
            const auto q = parse_warehouse_request(*c);
            CHECK(q && q->id.record == 108646121231993LL && q->access_config == kAccessClerk && q->storage_type == kStorageGeneral, "the fetch names the node, the config 7 and the general storage");
        }
    }
    if (const auto it = rec.find("request_visit"); it != rec.end()) {
        const auto calls = calls_of(it->second, "AoCPlayerControllerBP_C");
        const RpcCall* c = calls ? first_call(*calls, "ServerVisitWarehouse", "the visit") : nullptr;
        if (c) {
            CHECK(c->param_bits == 75, "the visit: 75 bits");
            const auto q = parse_warehouse_request(*c);
            CHECK(q && q->id.record == 6064633652044300331LL && q->access_config == kAccessClerk, "the visit names the node and the config 7");
        }
    }

    // the containers' empty payloads: an 8 x 16 tab, an 8-slot chunk (a list), a 6 x 8 cargo tab
    std::printf("-- the containers' payloads as recorded\n");
    check_payload(rec, "payload_tab_empty", 128, 8, 16);
    check_payload(rec, "payload_chunk_empty", 8, 0, 0);
    check_payload(rec, "payload_cargo_empty", 48, 6, 8);
}

void test_schema() {
    std::printf("-- the field set\n");
    const schema::RpcSchema& open = rpc_of("AoCPlayerControllerBP_C", "ClientOpenWarehouseWindow");
    for (const char* p : {"WarehouseId.Area", "WarehouseId.AreaRecordGuid", "StorageAccessConfig"}) CHECK(rpc_command(open, p) != nullptr, (std::string("the window RPC has ") + p).c_str());
    const schema::RpcSchema& sum = rpc_of("AoCPlayerControllerBP_C", "ClientReceiveWarehouseSummary");
    for (const char* p : {"Summary.Id.Area", "Summary.Id.AreaRecordGuid", "Summary.ResourceWarehouseStorageDefRecordGuid", "Summary.CrateWarehouseStorageDefRecordGuid", "Summary.ItemChunkSummaries",
                          "Summary.MaterialTabSummaries", "Summary.CrateTabSummaries"}) {
        CHECK(rpc_command(sum, p) != nullptr, (std::string("the summary RPC has ") + p).c_str());
    }

    // a summary built here reads back the same
    WarehouseSummary s;
    s.id = WarehouseId{kWarehouseAreaNode, 108646121231993LL};
    s.resource_def = 6064631794324733952LL;
    s.crate_def = 6064632025620086784LL;
    s.chunks.push_back({6064631806178426880LL, true, {}});
    s.chunks.push_back({6064631901618372609LL, false, {{kGold, kCurrencyType, 5.0}}});
    s.tabs.push_back({6064631957531525120LL, true, {}});
    s.crates.push_back({6064631963946647553LL, true, {}});
    const auto params = warehouse_summary_params(sum, s);
    CHECK(params.has_value(), "a summary builds parameters");
    if (params) {
        RpcCall call;
        call.rpc = &sum; call.params = *params; call.decoded = true;
        const auto back = parse_warehouse_summary(call);
        CHECK(back && *back == s, "built parameters parse back to the same summary");
    }
    // an entry that lists more costs than the codec writes is refused, and a bool above 1 does not read
    WarehouseSummaryEntry crowded;
    crowded.costs.resize(kMaxWarehouseCosts + 1);
    CHECK(!warehouse_entry_fits(crowded), "an entry of too many costs does not fit");

    // the two container classes (generated from the client's class layout) and the owner's lists that name them
    std::printf("-- the generated schemas\n");
    const schema::ClassSchema* tab = schema::gen::find_class_schema("UWarehouseResourceStorageComponent");
    const schema::ClassSchema* chunk = schema::gen::find_class_schema("UNodeItemStorageComponent");
    CHECK(tab && chunk, "both container classes have a schema");
    if (tab) {
        for (const char* p : {"ItemLocationType", "WarehouseId.Area", "WarehouseId.AreaRecordGuid", "PlayerGuid.A", "PlayerGuid.D", "ResourceStorageComponentIndex", "WarehouseStorageMaterialTabRecordGuid",
                              "SpecializedWarehouseTabDetailsRecordGuid", "StorageType", "bWithdrawOnly", "bReadOnly", "bIsMemberStorage"}) {
            CHECK(tab->command(p) != nullptr, (std::string("the tab class has ") + p).c_str());
        }
        CHECK(tab->command("bReadOnly") && tab->command("bReadOnly")->handle == 16, "bReadOnly is handle 16");
    }
    if (chunk) {
        for (const char* p : {"ItemLocationType", "NodeGuid", "PlayerGuid.A", "PlayerGuid.D", "WarehouseStorageItemChunkRecordId", "bWithdrawOnly"}) {
            CHECK(chunk->command(p) != nullptr, (std::string("the chunk class has ") + p).c_str());
        }
    }
    const schema::ClassSchema& owner = schema_named("UOwnerInfoComponent");
    for (const char* p : {"NodeItemContainers", "NodeResourceWarehouse.ActiveComponents", "NodeCrateWarehouse.ActiveComponents", "WarehouseMaterialTabsByIds"}) {
        CHECK(owner.command(p) != nullptr, (std::string("OwnerInfo has ") + p).c_str());
    }
}

}  // namespace

int main() {
    test_schema();
    test_recorded();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

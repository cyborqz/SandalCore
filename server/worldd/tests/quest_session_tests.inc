// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/quest_session_tests.inc  (included by test_worldd_area_session.cpp)
//
//  Quests, slice 1: taking a quest, on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB; the tests skip without it or without the quest tables).
//
//    * The data: every id the offers, the accept nodes, the zones and the item links name is a record of the data, and every one of the quests starts and fits the component's arrays.
//    * A whole offer through the real session, the real actor view and the fake client: the click on an NPC that offers a quest opens its offer talk, the lines lead to the node
//      that offers the quest, the quest panel is queued on top of it, a decline and a close take nothing, Accept takes the quest (the quest component's blocks: the cache item and
//      the variables), the log is saved, the NPC offers nothing more; Abandon deletes both items; taking it again makes a new instance; a relog gives the saved instance back with the
//      same guid.
//    * The client's rules, checked on OUR stream with the repo's own readers (they read the recorded blocks bit for bit: libs/aocnet/tests/test_repl_quest.cpp):
//        K1  every id we send is a record of the data     K5  instance guids are unique, stable across a relog, the parent is 0
//        K6  an instance's objectives are the quest's own, in its order, in the states its records give
//        K7  the fast arrays' keys rise and a removal is a deletion of the id the item was added with
//        K8  the quest component's blocks go on the owner's PlayerController channel and on no other
//        K9  the variables' keys are the hash of the names, in the game's order     K11  a quest that cannot be abandoned is refused, and an abandon takes the quest's items
//    * Refusals change nothing; the GM commands (the inbox, the client's own debug calls); a start by the zone of influence the character stands in (Welcome to Verra).
// ============================================================================
#include "quest/quest_book.h"
#include "quest/quest_log.h"
#include "quest/quest_service.h"
#include "quest/quest_wire.h"
#include "world/zoi_surface.h"

#include <mutex>
#include <span>
#include <thread>

namespace qs = aoc::worldd::quest;

constexpr int64_t kSoshaInstance = 6064634562958262298ll;       // the population instance of Sosha Terim, who offers Corrupted Memories
constexpr int64_t kSoshaRoot = 6064634563434774564ll;
constexpr int64_t kSoshaOfferTop = 6064634563458629669ll;       // the node her talk starts at while the quest is offered
constexpr int64_t kAcceptNode = 6064634569414541543ll;          // the node of her chain that offers the quest
constexpr int64_t kCorruptedMemories = 6064634563262021659ll;
constexpr int64_t kCorruptedFirstObjective = 6064634563268640796ll;
constexpr int64_t kOfTheRootways = 6064634760119582720ll;
constexpr int64_t kWelcomeToVerra = 6064634641390699452ll;
constexpr int64_t kRainOfRocks = 6064634349883364413ll;         // a quest with a bound item: the Goblin Rock Sack
constexpr int64_t kRockSack = 6064632196779935710ll;
constexpr int64_t kMissingQuest = 6064633689987481604ll;       // named by 7 offers and 2 offer nodes, and in no quest table
constexpr int64_t kStartingAreaZoi = 6064629127135166465ll;     // the zone that starts Welcome to Verra
constexpr double  kStartingAreaX = -819456, kStartingAreaY = 624384;   // a cell of that zone
constexpr double  kJoevaX = -1084437, kJoevaY = 503729;                // 4 m from where Sosha Terim stands in Joeva

/// The quest service in memory.
class FakeQuestApi : public svc::QuestApi {
public:
    svc::QuestLogResult load_log(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mu_);
        svc::QuestLogResult r;
        r.status = svc::CallStatus::Ok;
        r.log = logs_[id];
        return r;
    }
    svc::CallStatus save_log(const std::string& id, const svc::SavedQuestLog& log) override {
        std::lock_guard<std::mutex> lock(mu_);
        logs_[id] = log;
        ++saves_;
        return svc::CallStatus::Ok;
    }
    svc::CallStatus take_commands(const std::string& id, std::vector<svc::QuestCommand>& out) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto& box = inbox_[id];
        out.insert(out.end(), box.begin(), box.end());
        box.clear();
        return svc::CallStatus::Ok;
    }
    void queue(const std::string& id, std::string kind, std::string quest = {}, std::string name = {}, int64_t value = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        svc::QuestCommand c;
        c.id = ++next_;
        c.kind = std::move(kind);
        c.quest = std::move(quest);
        c.name = std::move(name);
        c.value = value;
        inbox_[id].push_back(std::move(c));
    }
    svc::SavedQuestLog log(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu_);
        return logs_[id];
    }
    size_t saves() {
        std::lock_guard<std::mutex> lock(mu_);
        return saves_;
    }

private:
    std::mutex mu_;
    std::map<std::string, svc::SavedQuestLog> logs_;
    std::map<std::string, std::vector<svc::QuestCommand>> inbox_;
    size_t  saves_ = 0;
    int64_t next_ = 0;
};

struct QuestTestData {
    content::QuestData      quests;
    std::optional<dl::Book> book;
};

/// A character who has been through the starting area already (its variable is set): the zone does not start Welcome to Verra for it again.  The default spawn, where a new character
/// starts, is inside that zone, so every test that does not test the zone gives its character this.
static svc::SavedQuestLog log_after_start() {
    svc::SavedQuestLog log;
    svc::SavedVariable v;
    v.name = "WelcomeToVerra";
    v.value = 1;
    log.variables.push_back(v);
    return log;
}

/// The real data: the conversations with the nodes that offer a quest, and the quests; nullopt (with the reason) when there is no file or it has no quest tables.
static std::optional<QuestTestData> quest_test_data(std::string& why) {
    const std::string path = dialogue_db_path();
    if (path.empty()) { why = "no content.db (dist/content/content.db or AOC_CONTENT_DB)"; return std::nullopt; }
    auto db = content::ContentDb::open(path, why);
    if (!db) return std::nullopt;
    auto dialogue = db->dialogue_data(why);
    if (!dialogue) return std::nullopt;
    auto quests = db->quest_data(why);
    if (!quests) return std::nullopt;
    QuestTestData out;
    out.book.emplace(std::move(*dialogue));
    out.book->set_accepts(quests->accepts);
    out.quests = std::move(*quests);
    return out;
}

/// A session of a character with quests: Sosha Terim stands 8.5 m away.
struct QuestHarness : Harness {
    FakeQuestApi&                    api;
    std::unique_ptr<qs::QuestWorker> worker;
    uint64_t                         talker = 0;
    bool                             ended = false;

    QuestHarness(FakeQuestApi& api_, const QuestTestData& data, svc::SavedQuestLog log, svc::Character r = cyb_record(), bool gm = false) : Harness(std::move(r)), api(api_) {
        record.quest_log = std::move(log);
        qs::QuestWorker::Config wc;
        wc.poll_interval = milliseconds(20);
        worker = std::make_unique<qs::QuestWorker>(api, wc);
        fx.world->set_dialogue(*data.book);
        fx.world->set_quests(qs::QuestBook(data.quests));
        fx.world->set_quest_worker(worker.get());
        fx.world->set_gm_commands(gm);
        const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
        if (!vendor_t) { std::printf("  [FAIL] no vendor template\n"); std::exit(2); }
        talker = fx.add(*vendor_t, kCx + 800, kCy + 300, false, 0, "Sosha Terim", kSoshaRoot);   // 8.5 m
        if (world::WorldActor* a = fx.world->registry().find(talker)) a->npc.instance_guid = kSoshaInstance;
        start();
    }
    ~QuestHarness() override { end(); }
    /// The character leaves: the session saves what it holds, and the worker's thread gets it done.
    void end() {
        if (ended) return;
        ended = true;
        s->finish("the test ends");
        worker->flush(seconds(3));
    }
    /// The tick queues the save and the worker's thread does it.
    void saved() {
        run(milliseconds(100));
        worker->flush(seconds(3));
    }
    /// The worker polls the inbox; the next tick hands the commands to the session, which acts and queues the save.
    void commands() {
        std::this_thread::sleep_for(milliseconds(150));
        saved();
    }
    void click() { client_request(interact(fx.world->registry().find(talker)->npc.interact_component), pawn_ch); }
};

// ---- reading our stream -------------------------------------------------------------------------------------------------------------

using Cmds = std::span<const schema::RepCommand>;

struct QuestBlock {
    std::vector<repl::FastArrayField> fields;
    size_t                            bits = 0;
};

/// The quest component's blocks of one delivered chain: an update from bit 0; an open (SerializeNewActor first) found by trying where the blocks start, as the other readers of this
/// file do.  A block counts only when its fields read exactly to the end of its payload.
static std::vector<QuestBlock> quest_blocks_of(const fake::Delivered& d, const emit::FIntrepidNetworkGUID& guid, const schema::ClassSchema& cls) {
    std::vector<QuestBlock> out;
    const size_t end = d.content.bit_pos();
    if (d.close || end == 0) return out;
    for (size_t start = 0; start < (d.open ? 4000u : 1u) && start < end; ++start) {
        const auto blocks = repl::read_content_blocks(d.content.data(), end, start);
        if (!blocks || blocks->empty()) continue;
        if (d.open && !(*blocks)[0].header.is_actor) continue;
        for (const repl::BlockView& b : *blocks) {
            if (b.header.is_actor || !(b.header.object == guid)) continue;
            size_t pos = b.payload_pos;
            auto f = repl::read_fast_array_fields(cls, d.content.data(), b.payload_pos + b.payload_bits, pos);
            if (f) out.push_back({std::move(*f), b.payload_bits});
        }
        if (!out.empty()) return out;
    }
    return out;
}

/// The quest component's blocks among the chains got[from..] that went out on the PlayerController's channel, in order; `others` counts those found on any other channel.
static std::vector<QuestBlock> quest_blocks_since(const Harness& h, size_t from, size_t* others = nullptr) {
    std::vector<QuestBlock> out;
    const schema::ClassSchema* cls = qs::component_schema();
    if (!cls) return out;
    const emit::FIntrepidNetworkGUID guid = h.s->bootstrap().setup().guids.quest;
    for (size_t i = from; i < h.got.size(); ++i) {
        auto blocks = quest_blocks_of(h.got[i], guid, *cls);
        for (QuestBlock& b : blocks) {
            if (h.got[i].channel == h.pc_ch) out.push_back(std::move(b));
            else if (others) ++*others;
        }
    }
    return out;
}

static const schema::RepCommand* cmd_at(Cmds cmds, std::string_view path) {
    for (const schema::RepCommand& c : cmds) {
        if (path == c.path) return &c;
    }
    return nullptr;
}
static const repl::Value* value_at(Cmds cmds, const repl::ElementState& es, std::string_view path) {
    const schema::RepCommand* c = cmd_at(cmds, path);
    if (!c) return nullptr;
    const auto it = es.values.find(c->handle);
    return it == es.values.end() ? nullptr : &it->second;
}
static int64_t int_at(Cmds cmds, const repl::ElementState& es, std::string_view path) {
    const repl::Value* v = value_at(cmds, es, path);
    if (!v) return INT64_MIN;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return INT64_MIN;
}
/// An FGuid member (its four words) as 32 hex digits; "missing" when a word is not in the item.
static std::string guid_at(Cmds cmds, const repl::ElementState& es, const std::string& prefix) {
    qs::Guid128 g;
    uint32_t* w[4] = {&g.a, &g.b, &g.c, &g.d};
    const char* const letters[4] = {".A", ".B", ".C", ".D"};
    for (int i = 0; i < 4; ++i) {
        const int64_t v = int_at(cmds, es, prefix + letters[i]);
        if (v == INT64_MIN) return "missing";
        *w[i] = static_cast<uint32_t>(v);
    }
    return g.hex();
}
/// The element commands of the array member at `path`.
static Cmds nested_at(const repl::FastArrayLayout& layout, std::string_view path) {
    const schema::RepCommand* c = cmd_at(layout.element, path);
    if (!c || c->type != schema::PropType::DynamicArray || c->elem_end <= c->elem_begin || c->elem_end > layout.table.size()) return {};
    return layout.table.subspan(c->elem_begin, c->elem_end - c->elem_begin);
}

static const repl::FastArrayField* field_named(const std::vector<repl::FastArrayField>& fields, const schema::ClassSchema& cls, const char* property) {
    const auto layout = repl::fast_array_layout(cls, property);
    if (!layout) return nullptr;
    for (const repl::FastArrayField& f : fields) {
        if (f.field == layout->field) return &f;
    }
    return nullptr;
}

/// What a quest block says of one quest: its cache item's members and its variables.
struct SeenQuest {
    int64_t                                      record = 0;
    std::string                                  instance, parent;
    int64_t                                      locality = -1;
    int32_t                                      cache_id = -1, vars_id = -1;
    std::vector<std::pair<std::string, int64_t>> objectives;      // instance guid, record
    std::vector<std::pair<uint64_t, int32_t>>    variables;       // (hash, value) in the order sent
    std::string                                  variables_key;   // the guid the variables item is keyed by
};

/// The quests a block adds or replaces, read from its two arrays by the layouts of the component.
static std::vector<SeenQuest> quests_in(const QuestBlock& b) {
    std::vector<SeenQuest> out;
    const schema::ClassSchema* cls = qs::component_schema();
    const repl::FastArrayField* cache = cls ? field_named(b.fields, *cls, "ReplicatedQuestCache") : nullptr;
    const repl::FastArrayField* vars = cls ? field_named(b.fields, *cls, "RepQuestVariables") : nullptr;
    const auto lc = cls ? repl::fast_array_layout(*cls, "ReplicatedQuestCache") : std::nullopt;
    const auto lv = cls ? repl::fast_array_layout(*cls, "RepQuestVariables") : std::nullopt;
    if (!cache || !lc) return out;
    const Cmds objective_cmds = nested_at(*lc, "Quests.Objectives");
    for (const auto& ch : cache->delta.changed) {
        const repl::ElementState& item = ch.values;
        SeenQuest q;
        q.cache_id = ch.id;
        q.record = int_at(lc->element, item, "Quests.RecordId");
        q.instance = guid_at(lc->element, item, "Quests.InstanceGuid");
        q.parent = guid_at(lc->element, item, "Quests.ParentInstanceGuid");
        q.locality = int_at(lc->element, item, "Quests.Locality");
        if (const repl::Value* ov = value_at(lc->element, item, "Quests.Objectives")) {
            if (const auto* list = ov->get_if<repl::ArrayValue>()) {
                for (const repl::ElementState& e : list->elements) q.objectives.push_back({guid_at(objective_cmds, e, "Objectives.InstanceGuid"), int_at(objective_cmds, e, "Objectives.RecordId")});
            }
        }
        out.push_back(std::move(q));
    }
    if (vars && lv) {
        for (const auto& ch : vars->delta.changed) {
            const std::string key = guid_at(lv->element, ch.values, "Items.Key");
            for (SeenQuest& q : out) {
                if (q.instance != key) continue;
                q.vars_id = ch.id;
                q.variables_key = key;
                if (const repl::Value* hv = value_at(lv->element, ch.values, "Items.Value")) {
                    if (const auto* hashed = hv->get_if<repl::HashedVariable>()) q.variables = hashed->variables;
                }
            }
        }
    }
    return out;
}

/// The variables of a quest the way the game lists them: the quest's four, then each objective's seven.
static std::vector<uint64_t> expected_variable_keys(const content::QuestRow& def) {
    std::vector<uint64_t> keys;
    for (const char* s : {"Completed", "Active", "Failed", "Paused"}) keys.push_back(qs::city_hash64(qs::quest_variable_name(def.guid, s)));
    for (const content::QuestObjectiveRow& o : def.objectives) {
        for (const char* s : {"Completed", "Active", "Failed", "Succeeded", "Hidden", "Count", "Awake"}) keys.push_back(qs::city_hash64(qs::objective_variable_name(def.guid, o.guid, s)));
    }
    return keys;
}

// ---- the client's calls ----------------------------------------------------------------------------------------------------------------------

static BunchWriter abandon_rpc(const std::vector<qs::Guid128>& guids, uint64_t state = 7) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerAbandonQuests");
    repl::RpcParams params;
    repl::ArrayValue list;
    for (const qs::Guid128& g : guids) {
        repl::ElementState e;
        e.values[1] = repl::Value::i64(static_cast<int32_t>(g.a));
        e.values[2] = repl::Value::i64(static_cast<int32_t>(g.b));
        e.values[3] = repl::Value::i64(static_cast<int32_t>(g.c));
        e.values[4] = repl::Value::i64(static_cast<int32_t>(g.d));
        list.elements.push_back(std::move(e));
    }
    if (!rpc || !repl::set_rpc_param(*rpc, params, "QuestGuids", repl::Value(std::move(list))) || !repl::set_rpc_param(*rpc, params, "QuestState", repl::Value::u64(state))) return BunchWriter();
    return actor_rpc(pc, "ServerAbandonQuests", params);
}

static BunchWriter blacklist_rpc(const qs::Guid128& g, bool on) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerSetQuestBlacklisted");
    repl::RpcParams params;
    if (!rpc) return BunchWriter();
    const char* const names[4] = {"QuestInstanceGuid.A", "QuestInstanceGuid.B", "QuestInstanceGuid.C", "QuestInstanceGuid.D"};
    const uint32_t words[4] = {g.a, g.b, g.c, g.d};
    for (int i = 0; i < 4; ++i) {
        if (!repl::set_rpc_param(*rpc, params, names[i], repl::Value::i64(static_cast<int32_t>(words[i])))) return BunchWriter();
    }
    if (!repl::set_rpc_param(*rpc, params, "bSetBlacklisted", repl::Value::boolean(on))) return BunchWriter();
    return actor_rpc(pc, "ServerSetQuestBlacklisted", params);
}

static BunchWriter sub_dialogue_response(repl::SubDialogueResponse answer) {
    repl::DialogueResponse r;
    r.response_type = static_cast<uint8_t>(repl::DialogueResponseType::SubDialogue);
    r.quest_response = static_cast<uint8_t>(answer);
    r.node_type_id = repl::kDialogueNodeTypeId;
    return dialogue_choice(r);
}

static BunchWriter debug_start_rpc(int64_t quest, bool bypass) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerDebugStartQuest");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "Guid", repl::Value::i64(quest)) || !repl::set_rpc_param(*rpc, params, "bBypassActivationRules", repl::Value::boolean(bypass))) return BunchWriter();
    return actor_rpc(pc, "ServerDebugStartQuest", params);
}

/// Walks the offer talk with the first shown line of each node until the quest panel is open (at most 12 steps).  The blocks of the last step are returned.
static bool walk_to_offer(QuestHarness& h, ClientDialogue& client, std::vector<PcBlock>& last) {
    for (int step = 0; step < 12 && !h.s->talk().sub(); ++step) {
        const auto& st = h.s->talk().state();
        if (st.lines.empty()) return false;
        const size_t from = h.got.size();
        h.client_request(dialogue_choice(choice_of(st.lines.front().node, st.lines.front().index)), h.pc_ch);
        last = pc_blocks_since(h, from);
        for (const auto& b : last) client.apply(b.state);
    }
    return h.s->talk().sub().has_value();
}

/// What the quest panel of a block says (QueuedSubDialogues): how many are queued and, for one, its type, quest, preview quest, node and the objectives the preview lists.
struct SeenPanel {
    size_t               count = 0;
    int64_t              type = -1, quest = 0, preview_quest = 0, node = 0, pending_next = -1;
    std::vector<int64_t> initial;
};
static SeenPanel panel_of(const PcBlock& b) {
    SeenPanel out;
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RepCommand* array = pc.command("CurrentDialogueInstance.QueuedSubDialogues");
    if (!array) return out;
    const repl::Value* v = b.state.get(array->handle);
    const auto* list = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!list) return out;
    out.count = list->elements.size();
    if (list->elements.size() != 1) return out;
    const Cmds elems = pc.elements(*array);
    const repl::ElementState& e = list->elements[0];
    out.type = int_at(elems, e, "QueuedSubDialogues.SubDialogueType");
    out.quest = int_at(elems, e, "QueuedSubDialogues.QuestId.Guid");
    out.preview_quest = int_at(elems, e, "QueuedSubDialogues.QuestPreview.QuestId.Guid");
    out.node = int_at(elems, e, "QueuedSubDialogues.SubDialogueNodeId.Guid");
    out.pending_next = int_at(elems, e, "QueuedSubDialogues.PendingNextNode.Guid");
    if (const repl::Value* iv = value_at(elems, e, "QueuedSubDialogues.QuestPreview.InitialObjectives")) {
        if (const auto* items = iv->get_if<repl::ArrayValue>()) {
            for (const repl::ElementState& it : items->elements) {
                const auto g = it.values.find(1);
                const auto* x = g == it.values.end() ? nullptr : g->second.get_if<int64_t>();
                if (x) out.initial.push_back(*x);
            }
        }
    }
    return out;
}

static bool has_handle(const std::vector<PcBlock>& blocks, uint16_t handle) {
    for (const PcBlock& b : blocks) {
        for (const uint16_t x : b.handles) {
            if (x == handle) return true;
        }
    }
    return false;
}

// ---- the data -----------------------------------------------------------------------------------------------------------------------------

static void test_quest_data() {
    std::printf("quests: every id the data names, and every quest on the wire\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    const qs::QuestBook qb(data->quests);
    const dl::Book& book = *data->book;
    size_t objectives = 0;
    std::set<int64_t> objective_ids;
    size_t shared_objectives = 0;
    for (const auto& [guid, q] : data->quests.quests) {
        objectives += q.objectives.size();
        for (const content::QuestObjectiveRow& o : q.objectives) shared_objectives += objective_ids.insert(o.guid).second ? 0 : 1;
    }
    CHECK(qb.quests() == 3026 && objectives == 10516, "the book holds the data's 3,026 quests with 10,516 objectives");
    std::printf("  [info] %zu objective records are named by more than one quest (the data repeats them across quests)\n", shared_objectives);

    size_t offers = 0, no_quest = 0, no_node = 0, with_node = 0;
    std::set<int64_t> missing;
    for (const auto* m : {&data->quests.instance_offers, &data->quests.asset_offers}) {
        for (const auto& [source, list] : *m) {
            for (const content::QuestOfferRow& o : list) {
                ++offers;
                with_node += o.node != 0 ? 1 : 0;
                no_quest += qb.quest(o.quest) ? 0 : 1;
                if (!qb.quest(o.quest)) missing.insert(o.quest);
                no_node += (o.node != 0 && !book.node(o.node)) ? 1 : 0;
            }
        }
    }
    std::printf("  [info] %zu offers: %zu name a node the talk starts at, %zu name a quest the data lacks, %zu a node the book lacks\n", offers, with_node, no_quest, no_node);
    CHECK(offers == 446 && no_node == 0, "K1: the 446 offers name a node of the data wherever they name one");
    CHECK(no_quest == 7 && missing == std::set<int64_t>({kMissingQuest}), "K1: all but 7 of the offers name quests of the data; those 7 name one quest the design data lacks");
    {
        dl::Facts facts;
        dl::Evaluator ev(book, facts);
        CHECK(qb.quest(kMissingQuest) == nullptr && qs::QuestLog(1).can_start(qb.quest(kMissingQuest), ev, false) == qs::Create::InvalidStaticData,
              "the server never offers it: the rules answer InvalidStaticData for a quest the data lacks");
    }

    size_t accepts = 0, a_no_node = 0, a_wrong_type = 0, a_no_quest = 0, a_bad_next = 0, a_bad_objective = 0;
    for (const auto& [node, a] : data->quests.accepts) {
        ++accepts;
        const content::QuestRow* q = qb.quest(a.quest);
        const content::DialogueNodeRow* n = book.node(node);
        a_no_node += n ? 0 : 1;
        a_wrong_type += (n && n->type != "AcceptQuest") ? 1 : 0;
        a_no_quest += q ? 0 : 1;
        a_bad_next += (a.on_success != 0 && !book.node(a.on_success)) ? 1 : 0;
        for (const int64_t o : a.objectives) {
            bool found = false;
            if (q) {
                for (const content::QuestObjectiveRow& x : q->objectives) found = found || x.guid == o;
            }
            a_bad_objective += found ? 0 : 1;
        }
    }
    std::printf("  [info] %zu offer nodes: %zu not in the book, %zu of another kind, %zu naming no quest, %zu leading to no node, %zu naming an objective of another quest\n", accepts, a_no_node, a_wrong_type, a_no_quest,
                a_bad_next, a_bad_objective);
    CHECK(accepts == 346 && a_no_node == 0 && a_wrong_type == 0 && a_no_quest == 2 && a_bad_next == 0 && a_bad_objective == 0,
          "K1: the 346 offer nodes are nodes of the data of the offer kind, lead on to nodes of the data and name objectives of their quests; two name a quest the design data lacks");

    size_t zone_links = 0, bad_zone = 0, item_links = 0, bad_items = 0;
    for (const auto& [zoi, list] : data->quests.zoi_quests) {
        for (const int64_t q : list) { ++zone_links; bad_zone += qb.quest(q) ? 0 : 1; }
    }
    for (const auto& [item, list] : data->quests.item_quests) {
        for (const content::QuestItemLink& l : list) { ++item_links; bad_items += (qb.quest(l.quest) && (l.kind == "delete_all" || l.kind == "delete_one" || l.kind == "abandon")) ? 0 : 1; }
    }
    CHECK(zone_links == 4 && bad_zone == 0 && item_links == 160 && bad_items == 0, "K1: the 4 zone links and the 160 item links name quests of the data");

    // every quest starts on a fresh log and its two items fit the component's arrays
    const schema::ClassSchema* cls = qs::component_schema();
    const auto lc = cls ? repl::fast_array_layout(*cls, "ReplicatedQuestCache") : std::nullopt;
    const auto lv = cls ? repl::fast_array_layout(*cls, "RepQuestVariables") : std::nullopt;
    CHECK(cls && lc && lv, "the build has the quest component with its two arrays");
    if (!lc || !lv) return;
    size_t not_fitting = 0, wrong_states = 0, wrong_counts = 0, quests_without_actions = 0;
    for (const auto& [guid, def] : data->quests.quests) {
        qs::QuestLog log(1);
        const qs::Started st = log.start(def, 0.0, 0);
        if (!st.instance) { ++not_fitting; continue; }
        const auto ci = qs::cache_item(*lc, *st.instance);
        const auto vi = qs::variables_item(*lv, *st.instance);
        if (!ci || !vi || !repl::item_matches(*lc, *ci) || !repl::item_matches(*lv, *vi)) { ++not_fitting; continue; }
        const auto vars = qs::instance_variables(*st.instance);
        wrong_counts += vars.size() == 4 + 7 * def.objectives.size() ? 0 : 1;
        if (def.start_actions.empty()) {
            ++quests_without_actions;
            // the objectives are in the states their records give: Awake and Hidden as the record says, Active with Awake
            for (size_t i = 0; i < def.objectives.size(); ++i) {
                const qs::ObjectiveState& o = st.instance->objectives[i];
                wrong_states += (o.guid.zero() || o.record != def.objectives[i].guid || o.awake != def.objectives[i].awake || o.hidden != def.objectives[i].hidden || o.active != def.objectives[i].awake) ? 1 : 0;
            }
        }
    }
    CHECK(not_fitting == 0 && wrong_counts == 0, "every one of the 3,026 quests starts, and its cache item and its variables (4 + 7 for each objective) fit the component's arrays");
    CHECK(wrong_states == 0 && quests_without_actions > 0, "K6: the objectives of a new quest are in the states their records give (the quests that have no start action)");
    std::printf("  [info] %zu quests have no start action\n", quests_without_actions);
}

// ---- the offer, the accept, the abandon and the relog ---------------------------------------------------------------------------------

static void test_quest_offer_accept_abandon() {
    std::printf("a quest offered by an NPC: decline, close, refuse, accept, abandon, take it again, relog\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    const qs::QuestBook qb(data->quests);
    const content::QuestRow* def = qb.quest(kCorruptedMemories);
    CHECK(def && def->objectives.size() == 5 && def->can_abandon && !def->repeatable && def->name == "RVR_HAR_QST_1000_CorruptedMemories", "the data has Corrupted Memories: 5 objectives, it can be abandoned and is not repeatable");
    const auto& offers = qb.offers(kSoshaInstance, 0);
    CHECK(offers.size() == 1 && offers[0].quest == kCorruptedMemories && offers[0].node == kSoshaOfferTop, "Sosha Terim offers it, her talk starting at the offer's node");
    const schema::ClassSchema* cls = qs::component_schema();
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RepCommand* queued_cmd = pc.command("CurrentDialogueInstance.QueuedSubDialogues");
    const schema::RepCommand* exhaust_cmd = pc.command("CurrentDialogueInstance.ExhaustBehavior");
    CHECK(def && cls && queued_cmd && exhaust_cmd, "the build has the quest component, the panel queue and ExhaustBehavior");
    if (!def || !cls || !queued_cmd || !exhaust_cmd) return;
    const uint16_t kQueue = queued_cmd->handle, kExhaust = exhaust_cmd->handle;

    FakeQuestApi api;
    QuestHarness h(api, *data, log_after_start());
    h.play();
    h.run(milliseconds(1500));
    CHECK(h.s->quests_on() && h.s->quest_log().instances().empty(), "the character's log loaded, no quest in it");
    size_t others = 0;
    CHECK(quest_blocks_since(h, 0, &others).size() >= 1 && others == 0, "the join sent the quest component's block (no quests: empty arrays), on the PlayerController's channel");
    ClientDialogue client;

    // the click: the offer talk
    size_t from = h.got.size();
    h.click();
    auto blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1 && h.s->talk().open() && h.s->talk().state().shown_node == kSoshaOfferTop && h.s->talk().state().greeting, "the click opens the offer talk at the node the offer names");
    if (blocks.size() == 1) {
        CHECK(blocks[0].handles == std::vector<uint16_t>({154, 156, 166, 170}), "the greeting carries CurrentChoices, ObjectSource, the node shown and bIsInGreeting, as every greeting does (K4)");
        client.apply(blocks[0].state);
        CHECK(client.lines.size() == 1 && client.shown_node == kSoshaOfferTop && client.lines[0].node == kSoshaOfferTop && client.lines[0].index == 0, "K2: one line, naming the node shown");
    }

    // the lines lead to the node that offers the quest: the panel is queued on top
    std::vector<PcBlock> last;
    CHECK(walk_to_offer(h, client, last), "the lines lead to the node that offers the quest, and the quest panel is queued");
    const dl::Talk::Sub sub = h.s->talk().sub().value_or(dl::Talk::Sub{});
    const content::DialogueNodeRow* offer_node = data->book->node(sub.node);
    CHECK(sub.quest == kCorruptedMemories && sub.node == kAcceptNode && offer_node && offer_node->type == "AcceptQuest", "K1: the panel names the quest and the offer node of the data");
    if (!last.empty()) {
        CHECK(has_handle(last, kQueue), "the block that shows the offer carries QueuedSubDialogues");
        const SeenPanel panel = panel_of(last.back());
        CHECK(panel.count == 1 && panel.type == 2 && panel.quest == kCorruptedMemories && panel.preview_quest == kCorruptedMemories && panel.node == kAcceptNode,
              "one QuestAccept (2) panel whose quest and preview are Corrupted Memories, at the offer node");
        CHECK(panel.initial == std::vector<int64_t>({kCorruptedFirstObjective}), "K1, K6: the preview lists the one objective the quest starts with, an objective of the quest");
    }
    // a line chosen while the panel is open: refused
    {
        const size_t refused = h.s->dialogue_refused();
        from = h.got.size();
        h.client_request(dialogue_choice(choice_of(h.s->talk().state().shown_node, 0)), h.pc_ch);
        CHECK(h.s->dialogue_refused() == refused + 1 && pc_blocks_since(h, from).empty() && h.s->talk().sub().has_value(), "a line chosen while the panel is open is refused, nothing changes");
    }

    // Decline: nothing is taken, the talk ends, the queue is cleared
    from = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Decline), h.pc_ch);
    blocks = pc_blocks_since(h, from);
    CHECK(h.s->quest_log().instances().empty() && h.s->quests_taken() == 0 && !h.s->talk().open(), "Decline takes nothing and ends the talk");
    CHECK(has_handle(blocks, kQueue) && has_handle(blocks, kExhaust) && quest_blocks_since(h, from).empty(), "the update carries the emptied queue and ExhaustBehavior, and no quest block");

    // Close at the panel: the same
    h.click();
    CHECK(h.s->talk().open() && walk_to_offer(h, client, last), "the quest is on offer again after a decline");
    from = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Close), h.pc_ch);
    CHECK(h.s->quest_log().instances().empty() && !h.s->talk().open() && quest_blocks_since(h, from).empty(), "Close at the panel takes nothing and ends the talk");

    // the GM gave the quest while the panel was open: the Accept is refused (the quest is in the log), nothing is doubled
    h.click();
    CHECK(h.s->talk().open() && walk_to_offer(h, client, last), "the panel opens once more");
    api.queue(kCyb, "give", "RVR_HAR_QST_1000_CorruptedMemories");
    h.commands();
    CHECK(h.s->quest_log().instances().size() == 1 && h.s->talk().sub().has_value(), "a GM gave the quest meanwhile: it is in the log and the panel is still open");
    const std::string given_guid = h.s->quest_log().instances().empty() ? std::string() : h.s->quest_log().instances()[0].guid.hex();
    {
        const size_t refused = h.s->quests_refused();
        from = h.got.size();
        h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Accept), h.pc_ch);
        CHECK(h.s->quests_refused() == refused + 1 && h.s->quests_taken() == 1 && h.s->quest_log().instances().size() == 1 && quest_blocks_since(h, from).empty(),
              "the Accept is refused (the quest is active already): still one instance, no quest block");
        CHECK(!h.s->talk().open() && has_handle(pc_blocks_since(h, from), kQueue), "... and the panel is closed and its queue cleared");
    }
    api.queue(kCyb, "abandon", "RVR_HAR_QST_1000_CorruptedMemories");
    h.commands();
    CHECK(h.s->quest_log().instances().empty(), "a GM takes it out again");

    // Accept
    h.click();
    CHECK(h.s->talk().open() && walk_to_offer(h, client, last), "the panel opens a last time");
    const size_t seen = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Accept), h.pc_ch);
    const auto q1 = quest_blocks_since(h, seen);
    const auto dlg = pc_blocks_since(h, seen);
    CHECK(h.s->quest_log().find(kCorruptedMemories) != nullptr && h.s->quests_taken() == 2, "Accept takes the quest");
    CHECK(q1.size() == 1 && q1[0].fields.size() == 3, "one quest component block with three arrays: the cache, the map pin of the first objective (a place to go to) and the variables");
    CHECK(!h.s->talk().open() && has_handle(dlg, kQueue) && !has_handle(dlg, kExhaust), "the talk ends, the update carries the emptied queue (ExhaustBehavior is not sent again: its value did not change since the first answer)");
    for (const auto& b : dlg) client.apply(b.state);
    CHECK(client.lines.empty() && client.shown_node == 0 && client.source == emit::FIntrepidNetworkGUID{}, "K3: the talk is cleared on the client");
    std::string instance_guid;
    int32_t cache_id = -1, vars_id = -1;
    if (q1.size() == 1) {
        const auto quests = quests_in(q1[0]);
        CHECK(quests.size() == 1, "one quest in the block");
        if (quests.size() == 1) {
            const SeenQuest& q = quests[0];
            instance_guid = q.instance;
            cache_id = q.cache_id;
            vars_id = q.vars_id;
            CHECK(q.record == kCorruptedMemories && qb.quest(q.record) != nullptr, "K1: the item is the quest of the data");
            CHECK(q.instance.size() == 32 && q.instance != std::string(32, '0') && q.instance != given_guid && q.parent == std::string(32, '0'), "K5: the instance has a guid of its own (not the earlier instance's) and no parent");
            CHECK(q.locality == 1, "the quest is Local (1)");
            bool same = q.objectives.size() == def->objectives.size();
            std::set<std::string> guids;
            for (size_t i = 0; same && i < q.objectives.size(); ++i) {
                same = same && q.objectives[i].second == def->objectives[i].guid;
                guids.insert(q.objectives[i].first);
            }
            CHECK(same && guids.size() == def->objectives.size() && guids.count(q.instance) == 0 && guids.count("missing") == 0, "K6: five objectives, the quest's own in its order, each with a guid of its own");
            std::vector<uint64_t> keys;
            std::map<uint64_t, int32_t> by_key;
            for (const auto& [k, v] : q.variables) { keys.push_back(k); by_key[k] = v; }
            CHECK(keys == expected_variable_keys(*def), "K9: the variables are keyed by the hash of their names, the quest's four and each objective's seven in the game's order");
            CHECK(q.vars_id >= 0 && q.variables_key == q.instance, "the variables item is keyed by the instance's guid");
            const auto on = [&](const std::string& name) { const auto it = by_key.find(qs::city_hash64(name)); return it == by_key.end() ? -1 : it->second; };
            const auto& o = def->objectives;
            CHECK(on(qs::quest_variable_name(kCorruptedMemories, "Active")) == 1 && on(qs::quest_variable_name(kCorruptedMemories, "Completed")) == 0, "the quest is Active, not Completed");
            CHECK(on(qs::objective_variable_name(kCorruptedMemories, o[0].guid, "Active")) == 1 && on(qs::objective_variable_name(kCorruptedMemories, o[0].guid, "Awake")) == 1 &&
                      on(qs::objective_variable_name(kCorruptedMemories, o[0].guid, "Hidden")) == 0,
                  "K6: the first objective is Active, Awake and not Hidden (the quest's start action activates it)");
            bool rest_hidden = true;
            for (size_t i = 1; i < o.size(); ++i) {
                rest_hidden = rest_hidden && on(qs::objective_variable_name(kCorruptedMemories, o[i].guid, "Hidden")) == 1 && on(qs::objective_variable_name(kCorruptedMemories, o[i].guid, "Active")) == 0 &&
                              on(qs::objective_variable_name(kCorruptedMemories, o[i].guid, "Awake")) == 0;
            }
            CHECK(rest_hidden, "K6: the other four are Hidden, not Awake and not Active");
        }
        // K7: each array's key rises
        const repl::FastArrayField* cache = field_named(q1[0].fields, *cls, "ReplicatedQuestCache");
        const repl::FastArrayField* vars = field_named(q1[0].fields, *cls, "RepQuestVariables");
        CHECK(cache && vars && cache->delta.changed.size() == 1 && vars->delta.changed.size() == 1 && cache->delta.deleted.empty() && vars->delta.deleted.empty(), "one item in each array, nothing deleted");
        CHECK(cache && vars && cache->delta.base_key > 0 && cache->delta.array_key > cache->delta.base_key && vars->delta.array_key > vars->delta.base_key,
              "K7: each array's key rises against the key the client has (the earlier quests of this session raised it)");
    }
    others = 0;
    quest_blocks_since(h, 0, &others);
    CHECK(others == 0, "K8: the quest component's blocks went out on the PlayerController's channel only");
    h.saved();
    svc::SavedQuestLog saved = api.log(kCyb);
    CHECK(saved.instances.size() == 1 && saved.instances[0].record_id == kCorruptedMemories && saved.instances[0].instance_guid == instance_guid && saved.instances[0].objectives.size() == 5,
          "the log was saved: the quest with the guid the client knows it by and its five objectives");

    // the NPC offers nothing more: its talk has nothing to say
    from = h.got.size();
    h.click();
    CHECK(pc_blocks_since(h, from).empty() && !h.s->talk().open(), "Sosha has nothing more to say to a character who has the quest: no window");

    // the Destiny window's toggle is accepted and ignored
    const std::optional<qs::Guid128> g = qs::Guid128::from_hex(instance_guid);
    from = h.got.size();
    h.client_request(blacklist_rpc(g.value_or(qs::Guid128{}), true), h.pc_ch);
    CHECK(quest_blocks_since(h, from).empty() && h.s->quest_log().instances().size() == 1, "ServerSetQuestBlacklisted is accepted and changes nothing");

    // Abandon
    const size_t abandoned_before = h.s->quests_abandoned();
    from = h.got.size();
    h.client_request(abandon_rpc({g.value_or(qs::Guid128{})}), h.pc_ch);
    const auto q2 = quest_blocks_since(h, from);
    CHECK(h.s->quest_log().instances().empty() && h.s->quests_abandoned() == abandoned_before + 1, "Abandon takes the quest out of the log");
    CHECK(q2.size() == 1 && q2[0].fields.size() == 2, "one quest component block with two arrays");
    if (q2.size() == 1) {
        const repl::FastArrayField* cache = field_named(q2[0].fields, *cls, "ReplicatedQuestCache");
        const repl::FastArrayField* vars = field_named(q2[0].fields, *cls, "RepQuestVariables");
        CHECK(cache && vars && cache->delta.deleted == std::vector<int32_t>({cache_id}) && cache->delta.changed.empty() && vars->delta.deleted == std::vector<int32_t>({vars_id}) && vars->delta.changed.empty(),
              "K7: both arrays delete the id the item was added with, and change nothing");
        CHECK(cache && cache->delta.base_key > 0 && cache->delta.array_key == cache->delta.base_key + 1, "K7: the array key rises by one, against the key the client has");
    }
    h.saved();
    CHECK(api.log(kCyb).instances.empty(), "the saved log is empty again");
    // a call that names no quest of the log changes nothing
    from = h.got.size();
    const size_t refused_before = h.s->quests_refused();
    h.client_request(abandon_rpc({qs::Guid128{1, 2, 3, 4}}), h.pc_ch);
    CHECK(quest_blocks_since(h, from).empty() && h.s->quests_refused() == refused_before + 1, "an abandon of a guid nobody has is refused: no block");

    // take it again: a new instance
    h.click();
    CHECK(h.s->talk().open() && h.s->talk().state().shown_node == kSoshaOfferTop, "the quest is on offer again after the abandon");
    ClientDialogue again;
    CHECK(walk_to_offer(h, again, last), "the lines lead to the offer again");
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Accept), h.pc_ch);
    const qs::QuestInstance* second = h.s->quest_log().find(kCorruptedMemories);
    CHECK(second && second->guid.hex() != instance_guid && second->guid.hex() != given_guid, "K5: the third instance has a guid of its own");
    const std::string second_guid = second ? second->guid.hex() : std::string();
    h.saved();

    // a relog: the session ends, a new one joins with the saved log, and the client is given the quest with the guid it knew it by
    h.end();
    saved = api.log(kCyb);
    CHECK(saved.instances.size() == 1 && saved.instances[0].instance_guid == second_guid, "the log the session ended with holds the quest");
    QuestHarness r(api, *data, saved);
    r.play();
    r.run(milliseconds(1500));
    CHECK(r.s->quests_on() && r.s->quest_log().instances().size() == 1 && r.s->quest_log().instances()[0].guid.hex() == second_guid, "K5: after a relog the quest has the same instance guid");
    bool in_join = false, objectives_same = false;
    for (const QuestBlock& b : quest_blocks_since(r, 0)) {
        for (const SeenQuest& q : quests_in(b)) {
            in_join = in_join || (q.instance == second_guid && q.record == kCorruptedMemories);
            objectives_same = objectives_same || (q.instance == second_guid && q.objectives.size() == 5 && q.objectives[0].second == def->objectives[0].guid);
        }
    }
    CHECK(in_join && objectives_same, "the join's quest block carries that quest with the same guid and its objectives");
    r.click();
    CHECK(!r.s->talk().open(), "and Sosha offers nothing to the relogged character either");
}

// ---- refusals change nothing ----------------------------------------------------------------------------------------------------------------

static void test_quest_refusals() {
    std::printf("quests: refusals change nothing\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    const qs::QuestBook qb(data->quests);
    FakeQuestApi api;
    // a character who has the quest already is not offered it, and an Accept that arrives anyway changes nothing
    svc::SavedQuestLog have;
    {
        qs::QuestLog log(7);
        log.start(*qb.quest(kCorruptedMemories), 0.5, 0);
        log.set_variable("WelcomeToVerra", 1);
        have = log.to_saved();
    }
    QuestHarness h(api, *data, have);
    h.play();
    h.run(milliseconds(1500));
    CHECK(h.s->quests_on() && h.s->quest_log().instances().size() == 1, "a character who has the quest joins with it");
    size_t from = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Accept), h.pc_ch);
    CHECK(quest_blocks_since(h, from).empty() && h.s->quest_log().instances().size() == 1 && h.s->quests_taken() == 0, "an Accept with no panel open changes nothing");
    from = h.got.size();
    h.click();
    CHECK(!h.s->talk().open() && pc_blocks_since(h, from).empty(), "the click on Sosha shows no window: the quest is in the log and her own talk has nothing to say");

    // Welcome to Verra cannot be abandoned: its record says so, and the server refuses it too
    const content::QuestRow* welcome = qb.quest(kWelcomeToVerra);
    CHECK(welcome && !welcome->can_abandon && welcome->objectives.size() == 9, "K11: the data says Welcome to Verra cannot be abandoned (9 objectives)");
    api.queue(kCyb, "give", "RVR_FTUE_QST_1400_WelcomeToVerra");
    h.commands();
    const qs::QuestInstance* w = h.s->quest_log().find(kWelcomeToVerra);
    CHECK(w != nullptr, "a GM give puts it in the log whatever its rules say");
    if (w) {
        const qs::Guid128 g = w->guid;
        from = h.got.size();
        const size_t refused = h.s->quests_refused();
        h.client_request(abandon_rpc({g}), h.pc_ch);
        CHECK(h.s->quest_log().find(kWelcomeToVerra) != nullptr && quest_blocks_since(h, from).empty() && h.s->quests_refused() == refused + 1, "K11: an abandon of it is refused by the server, and nothing changes");
        CHECK(h.s->quest_log().variable("WelcomeToVerra") == 1, "its start action set the player variable WelcomeToVerra");
        // a call that names one quest it may drop and one it may not drops the first only
        const qs::QuestInstance* cm = h.s->quest_log().find(kCorruptedMemories);
        from = h.got.size();
        if (cm) h.client_request(abandon_rpc({g, cm->guid}), h.pc_ch);
        CHECK(h.s->quest_log().find(kWelcomeToVerra) != nullptr && h.s->quest_log().find(kCorruptedMemories) == nullptr && quest_blocks_since(h, from).size() == 1, "a call naming both drops only the one that may be abandoned, in one block");
    }
    // the same rules for a quest given again: it is in the log, so it is refused and nothing doubles
    api.queue(kCyb, "give", "RVR_FTUE_QST_1400_WelcomeToVerra");
    from = h.got.size();
    h.commands();
    CHECK(h.s->quest_log().instances().size() == 1 && quest_blocks_since(h, from).empty(), "a second give of a quest in the log changes nothing");
    // a quest that does not exist, a quest that is not in the log, an unknown command
    api.queue(kCyb, "give", "NoSuchQuest");
    api.queue(kCyb, "abandon", "RVR_HAR_QST_1000_CorruptedMemories");
    api.queue(kCyb, "frobnicate", "RVR_HAR_QST_1000_CorruptedMemories");
    from = h.got.size();
    h.commands();
    CHECK(h.s->quest_log().instances().size() == 1 && quest_blocks_since(h, from).empty(), "a quest that is not in the data, a quest that is not in the log and an unknown command change nothing");
    // the client's own debug calls are refused when the world has no GM commands
    from = h.got.size();
    h.client_request(debug_start_rpc(kCorruptedMemories, true), h.pc_ch);
    CHECK(h.s->quest_log().find(kCorruptedMemories) == nullptr && quest_blocks_since(h, from).empty(), "the client's own debug start is refused when GM commands are off");
}

// ---- GM commands, the quest items ------------------------------------------------------------------------------------------------------

static void test_quest_gm() {
    std::printf("quests: the GM's commands, the quest items\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    const qs::QuestBook qb(data->quests);
    FakeQuestApi api;
    std::vector<svc::InventoryItem> items = kit();
    items.push_back(make_item(ItemContainer::Quest, 0, kRockSack, 300, 3));
    QuestHarness h(api, *data, log_after_start(), cyb_record(items), /*gm=*/true);
    h.play();
    h.run(milliseconds(1500));
    const auto sacks = [&] {
        int n = 0;
        for (const svc::InventoryItem& it : h.s->inventory().items()) n += it.item_record_id == kRockSack ? it.quantity : 0;
        return n;
    };
    CHECK(sacks() == 3, "the character carries three Goblin Rock Sacks in the quest bag");
    const content::QuestRow* rootways = qb.quest(kOfTheRootways);
    CHECK(rootways && !rootways->activation.empty(), "Of the Rootways has an activation rule (Welcome to Verra done and a player variable)");
    // give: the rule is bypassed
    size_t from = h.got.size();
    api.queue(kCyb, "give", "RVR_FTUE_QST_1415_OfTheRootways");
    h.commands();
    CHECK(h.s->quest_log().find(kOfTheRootways) != nullptr, "questctl give: Of the Rootways is in the log although its rule does not hold");
    CHECK(quest_blocks_since(h, from).size() == 1, "... and the client got one block");
    // finish: out of the log, counted
    api.queue(kCyb, "finish", "RVR_FTUE_QST_1415_OfTheRootways");
    h.commands();
    CHECK(h.s->quest_log().instances().empty() && h.s->quest_log().completions(kOfTheRootways) == 1, "questctl finish: out of the log, finished once");
    // a finished quest that is not repeatable is refused by the rules, whoever asks
    {
        dl::Facts facts;
        dl::Evaluator ev(*data->book, facts);
        CHECK(h.s->quest_log().can_start(rootways, ev, false) == qs::Create::QuestNotRepeatable && h.s->quest_log().can_start(rootways, ev, true) == qs::Create::QuestNotRepeatable,
              "it is not repeatable: refused by the rules, and a GM bypass skips only the activation rule");
    }
    // set-var, clear
    api.queue(kCyb, "set-var", {}, "PickedSamias", 1);
    h.commands();
    CHECK(h.s->quest_log().variable("PickedSamias") == 1, "questctl set-var PickedSamias 1");
    api.queue(kCyb, "clear");
    h.commands();
    CHECK(h.s->quest_log().completions(kOfTheRootways) == 0 && h.s->quest_log().variable("PickedSamias") == 0, "questctl clear: the finished quests and the variables are forgotten");

    // the client's own debug call
    from = h.got.size();
    h.client_request(debug_start_rpc(kCorruptedMemories, false), h.pc_ch);
    {
        const size_t sent = quest_blocks_since(h, from).size();
        const bool in_log = h.s->quest_log().find(kCorruptedMemories) != nullptr;
        if (!in_log || sent != 1) std::printf("  [info] after the debug start: %zu quest(s) in the log, %zu quest block(s) sent, GM RPCs seen %zu, answered %zu\n", h.s->quest_log().instances().size(), sent, h.s->gm_rpcs_received(), h.s->gm_rpcs_answered());
        CHECK(in_log && sent == 1, "ServerDebugStartQuest takes Corrupted Memories (GM commands on)");
    }
    // Of the Rootways by the debug call with the rules asked: its rule does not hold
    from = h.got.size();
    h.client_request(debug_start_rpc(kOfTheRootways, false), h.pc_ch);
    CHECK(h.s->quest_log().find(kOfTheRootways) == nullptr && quest_blocks_since(h, from).empty(), "ServerDebugStartQuest with the rules asked refuses Of the Rootways: its activation rule does not hold, nothing changes");
    from = h.got.size();
    h.client_request(debug_start_rpc(kOfTheRootways, true), h.pc_ch);
    CHECK(h.s->quest_log().find(kOfTheRootways) != nullptr && quest_blocks_since(h, from).size() == 1, "... and with the rules bypassed it takes it");

    // K5, K7: four quests in one log: every instance and objective guid is unique, and so is every replication id
    api.queue(kCyb, "give", "RVR_FTUE_QST_1400_WelcomeToVerra");
    api.queue(kCyb, "give", "CSQ_RVR_0031_ROR_RainOfRocks");
    from = h.got.size();
    h.commands();
    {
        std::set<std::string> guids;
        size_t expected = 0;
        for (const qs::QuestInstance& q : h.s->quest_log().instances()) {
            guids.insert(q.guid.hex());
            ++expected;
            for (const qs::ObjectiveState& o : q.objectives) { guids.insert(o.guid.hex()); ++expected; }
        }
        CHECK(h.s->quest_log().instances().size() == 4 && guids.size() == expected, "K5: four quests in the log, every instance and objective guid different");
        std::set<int32_t> cache_ids, vars_ids;   // (each array counts its ids on its own)
        for (const qs::QuestInstance& q : h.s->quest_log().instances()) { cache_ids.insert(q.cache_id); vars_ids.insert(q.vars_id); }
        CHECK(cache_ids.size() == 4 && vars_ids.size() == 4 && cache_ids.count(repl::kIndexNone) == 0 && vars_ids.count(repl::kIndexNone) == 0, "K7: every item of each array has a replication id of its own");
    }

    // K11: an abandon takes the quest's items: the quest bag loses the sacks, the client is told, the saved inventory has none
    const qs::QuestInstance* rain = h.s->quest_log().find(kRainOfRocks);
    CHECK(rain != nullptr, "Rain of Rocks is in the log");
    if (rain) {
        const qs::Guid128 g = rain->guid;
        from = h.got.size();
        h.client_request(abandon_rpc({g}), h.pc_ch);
        CHECK(h.s->quest_log().find(kRainOfRocks) == nullptr && sacks() == 0, "K11: the abandon took the quest out of the log, and the sacks out of the quest bag");
        size_t removes = 0;
        for (const repl::RpcCall& c : pc_calls_since(h, from)) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientReceiveStorageEvent") continue;
            const auto ev = repl::parse_storage_event(c);
            removes += (ev && ev->type == static_cast<uint8_t>(repl::StorageEventType::Remove) && static_cast<int64_t>(ev->item.record_id) == kRockSack) ? 1 : 0;
        }
        CHECK(removes == 1, "K11: the client was told with one Remove event for the sacks");
    }
    h.end();
    bool saved_has_sacks = false;
    for (const auto& inv : h.sink.inventories) {
        for (const svc::InventoryItem& it : inv) saved_has_sacks = saved_has_sacks || it.item_record_id == kRockSack;
    }
    CHECK(!h.sink.inventories.empty() && !saved_has_sacks, "K11: the inventory handed to the saver when the session ended has no sacks");
    CHECK(api.log(kCyb).instances.size() == 3, "the saved log holds the three quests that are left");
}

// ---- the zone that starts a quest ----------------------------------------------------------------------------------------------------

/// The dialogue conditions' view of a log: its variables and quest states.
struct LogFacts : dl::Facts {
    const qs::QuestLog& log;
    explicit LogFacts(const qs::QuestLog& l) : log(l) {}
    int64_t player_variable(const std::string& n) const override { return log.variable(n); }
    std::optional<int64_t> quest_count(int64_t quest, const std::string& query) const override { return log.quest_count(quest, query); }
};

static void test_quest_zone() {
    std::printf("quests: the starting area starts Welcome to Verra\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    CHECK(world::zoi_at(kStartingAreaX, kStartingAreaY) == static_cast<uint64_t>(kStartingAreaZoi), "the test spot is inside the zone that starts Welcome to Verra");
    CHECK(world::zoi_at(bootstrap::kDefaultSpawn.x, bootstrap::kDefaultSpawn.y) == static_cast<uint64_t>(kStartingAreaZoi), "so is the default spawn, where a new character starts");
    CHECK(world::zoi_at(kJoevaX, kJoevaY) != static_cast<uint64_t>(kStartingAreaZoi), "Joeva is not");
    const qs::QuestBook qb(data->quests);
    CHECK(qb.zone_quests(kStartingAreaZoi) == std::vector<int64_t>({kWelcomeToVerra}), "the data lists Welcome to Verra as the zone's quest");
    const content::QuestRow* welcome = qb.quest(kWelcomeToVerra);
    FakeQuestApi api;
    // a character who stands elsewhere gets nothing
    {
        svc::Character in_joeva = cyb_record();
        in_joeva.has_position = true;
        in_joeva.position = {kJoevaX, kJoevaY, bootstrap::kDefaultSpawn.z, 0.0f};
        QuestHarness h(api, *data, svc::SavedQuestLog{}, in_joeva);
        h.play();
        h.run(milliseconds(3500));
        CHECK(h.s->quest_log().instances().empty() && h.s->quests_taken() == 0, "a character outside the zone gets no quest");
    }
    // a character who has been through it (the variable is set) gets nothing either
    {
        QuestHarness h(api, *data, log_after_start());
        h.play();
        h.run(milliseconds(3500));
        CHECK(h.s->quest_log().instances().empty() && h.s->quests_taken() == 0, "a character in the zone whose variable is set gets no quest: it starts once");
    }
    // a new character, at the default spawn, gets the quest within a second of the phase-in, and only once
    QuestHarness h(api, *data, svc::SavedQuestLog{});
    h.play();
    h.run(milliseconds(3500));
    const qs::QuestInstance* w = h.s->quest_log().find(kWelcomeToVerra);
    CHECK(w != nullptr && welcome != nullptr, "a new character standing in the zone got Welcome to Verra");
    if (!w || !welcome) return;
    CHECK(h.s->quest_log().variable("WelcomeToVerra") == 1 && w->objectives.size() == 9 && h.s->quests_taken() == 1, "its start action set WelcomeToVerra, and all nine objectives are in the instance");
    h.run(milliseconds(3000));
    CHECK(h.s->quest_log().instances().size() == 1 && h.s->quests_taken() == 1, "the zone starts it once, not every second");
    const auto blocks = quest_blocks_since(h, 0);
    bool sent = false;
    for (const QuestBlock& b : blocks) {
        for (const SeenQuest& q : quests_in(b)) {
            sent = sent || (q.record == kWelcomeToVerra && q.objectives.size() == 9 && q.variables.size() == 4 + 7 * 9);
        }
    }
    CHECK(sent, "the client was sent it, with its nine objectives and sixty-seven variables");
    h.saved();
    CHECK(api.log(kCyb).instances.size() == 1 && api.log(kCyb).variables.size() == 1, "the quest and the variable were saved");
    // a GM abandon takes it out (the player cannot); its rule says once, so it is not offered again
    api.queue(kCyb, "abandon", "RVR_FTUE_QST_1400_WelcomeToVerra");
    h.commands();
    h.run(milliseconds(3000));
    CHECK(h.s->quest_log().instances().empty() && h.s->quests_taken() == 1, "a GM abandon takes it out, and the zone does not start it again");
    LogFacts facts(h.s->quest_log());
    dl::Evaluator ev(*data->book, facts);
    CHECK(h.s->quest_log().can_start(welcome, ev, false) == qs::Create::FailedActivationRules && h.s->quest_log().can_start(welcome, ev, true) == qs::Create::Ok,
          "the rule refuses it now (the variable is set); a GM start skips the rule");
}

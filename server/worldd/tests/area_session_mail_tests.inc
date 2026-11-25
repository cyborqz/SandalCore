// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_mail_tests.inc: the use of a mailbox on a connection opens the mail window with the mailbox's own base guid, and
//  a quest object that is not a mailbox opens no window (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs it
//  through aoc_tests::run; it is included from area_session_registered.inc)
// ============================================================================

constexpr int64_t test_mailbox_icd = 6064634013971120329ll;
constexpr int64_t test_mailbox_pid = 6064631686289752065ll;
constexpr int64_t test_mailbox_asset_guid = 6064631958580297729ll;
constexpr int64_t test_mailbox_instance_guid = 6064631958589210627ll;
constexpr int64_t test_mailbox_crate_icd = 6064634013971120999ll;
constexpr int64_t test_mailbox_crate_pid = 6064631686289752999ll;
constexpr int64_t test_mailbox_crate_asset_guid = 6064631958580297730ll;
constexpr int64_t test_mailbox_crate_instance_guid = 6064631958589210628ll;

/// The mailbox's package and a quest object's package (the second is under the quest folder, as the old allowlist knows it).
static const char* test_mailbox_path() { return "/Game/DesignAssets/Node/ServiceBuildings/Default/Warehouse/KAE/Mailbox.Default__Mailbox_C"; }
static const char* test_mailbox_crate_path() { return "/Game/DesignAssets/Interactables/Quest/Crate.Default__Crate_C"; }

/// A fixture with the item table the GM vendors read (the other tables stay the fixture's own).
static content_fixture::DbOptions test_mailbox_options() {
    content_fixture::DbOptions o;
    o.item_tables = true;
    return o;
}

/// The quest object and vendor rows the session reads: two interactables, and a GM vendor def so that the area load reaches the objects.
static bool test_mailbox_inject(const fs::path& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        std::printf("  [FAIL] test setup: cannot open %s\n", path.string().c_str());
        if (db) sqlite3_close(db);
        return false;
    }
    const char* sql = R"sql(
CREATE TABLE IF NOT EXISTS interactable (
    icd_guid INTEGER PRIMARY KEY, name TEXT NOT NULL, radius REAL NOT NULL, tolerance REAL NOT NULL, discovery REAL NOT NULL,
    require_los INTEGER NOT NULL, requirement TEXT NOT NULL, pid_guid INTEGER NOT NULL, channel REAL NOT NULL, busy REAL NOT NULL,
    allow_combat INTEGER NOT NULL, single_use INTEGER NOT NULL, delete_done INTEGER NOT NULL, delete_delay REAL NOT NULL,
    anim TEXT NOT NULL, grants TEXT NOT NULL, reward_table INTEGER NOT NULL, complete_objs TEXT NOT NULL, reveal_objs TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS interactable_actor_def (iad_guid INTEGER PRIMARY KEY, despawn_delay REAL NOT NULL, despawn_final INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS instance_iad (instance_guid INTEGER PRIMARY KEY, iad_guid INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS quest_item (item_guid INTEGER PRIMARY KEY, major INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS item_box (item_guid INTEGER PRIMARY KEY, table_guid INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS item_reward_selection (item_guid INTEGER PRIMARY KEY, table_guid INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS vendor_def (guid INTEGER PRIMARY KEY, name TEXT NOT NULL, operations INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS vendor_def_inventory (vendor_guid INTEGER NOT NULL, ord INTEGER NOT NULL, inventory_guid INTEGER NOT NULL, PRIMARY KEY (vendor_guid, ord)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS vendor_inventory (guid INTEGER PRIMARY KEY, name TEXT NOT NULL, hidden INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS vendor_listing (inventory_guid INTEGER NOT NULL, ord INTEGER NOT NULL, item_guid INTEGER NOT NULL, offers TEXT NOT NULL, hidden INTEGER NOT NULL, PRIMARY KEY (inventory_guid, ord)) WITHOUT ROWID;
INSERT OR REPLACE INTO interactable VALUES
    (6064634013971120329, 'ICD_OpenMailbox', 200.0, 30.0, 200.0, 0, '', 6064631686289752065, 0.0, 0.9, 1, 0, 0, 0.0, '', '[]', 0, '[]', '[]'),
    (6064634013971120999, 'ICD_OpenCrate', 200.0, 30.0, 200.0, 0, '', 6064631686289752999, 0.0, 0.9, 1, 0, 0, 0.0, '', '[]', 0, '[]', '[]');
INSERT OR REPLACE INTO vendor_def VALUES (9000000000000000001, 'PTR_ONLY_Vendor_TestVendor', 0);
)sql";
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::printf("  [FAIL] test setup: the object rows do not take: %s\n", error ? error : "?");
        sqlite3_free(error);
        sqlite3_close(db);
        return false;
    }
    sqlite3_close(db);
    return true;
}

static content::NpcAssetRow test_mailbox_asset_row(int64_t guid, const char* name, const char* path) {
    content::NpcAssetRow a;
    a.guid = guid;
    a.name = name;
    a.archetype_path = path;
    return a;
}

static content::NpcInstanceRow test_mailbox_instance_row(int64_t guid, const char* name, int64_t component, int64_t response) {
    content::NpcInstanceRow i;
    i.guid = guid;
    i.name = name;
    i.level_min = 1;
    i.level_max = 1;
    i.interact_component = component;
    i.interact_response = response;
    return i;
}

/// The four words of the actor's InteractableBaseGuid.
static repl::GuidValue test_mailbox_state_guid(const world::WorldActor& a) {
    const auto word = [&](const char* path) -> uint32_t {
        if (!a.actor.obj.schema) return 0;
        const repl::Value* v = a.actor.obj.state.get(*a.actor.obj.schema, path);
        const auto* n = v ? v->get_if<int64_t>() : nullptr;
        return n ? static_cast<uint32_t>(*n) : 0;
    };
    return repl::GuidValue{word("InteractableBaseGuid.A"), word("InteractableBaseGuid.B"), word("InteractableBaseGuid.C"), word("InteractableBaseGuid.D")};
}

/// The four words of the MailboxGuid parameter of a ClientOpenMailUI call; nullopt when the call is another one or the words do not read.
static std::optional<repl::GuidValue> test_mailbox_call_guid(const repl::RpcCall& c) {
    if (!c.rpc || std::string(c.rpc->name) != "ClientOpenMailUI") return std::nullopt;
    uint32_t words[4] = {};
    for (int i = 0; i < 4; ++i) {
        const std::string path = std::string("MailboxGuid.") + "ABCD"[i];
        const repl::Value* v = repl::rpcf::value_of(*c.rpc, c.params, path);
        const auto* n = v ? v->get_if<int64_t>() : nullptr;
        if (!n) return std::nullopt;
        words[i] = static_cast<uint32_t>(*n);
    }
    return repl::GuidValue{words[0], words[1], words[2], words[3]};
}

/// The character service's mail store for the mailbox tests: the mails the character waits for.
struct test_mailbox_sink : RecordingSink {
    std::vector<svc::Mail> mails;
    svc::MailsResult load_mails(const std::string&) override {
        svc::MailsResult result;
        result.status = svc::CallStatus::Ok;
        result.mails = mails;
        return result;
    }
};

/// One mail from the town's post, unread unless its status says otherwise.
static svc::Mail test_mailbox_mail(uint32_t n, const char* subject, int32_t status = 0) {
    svc::Mail mail;
    mail.id = guid_n(n);
    mail.sender = "Lionhold Post";
    mail.subject = subject;
    mail.body = "The post left this for you at the mailbox.";
    mail.status = status;
    mail.issued_at_ms = 1'700'000'000'000ll;
    mail.expires_at_ms = 1'700'604'800'000ll;
    return mail;
}

/// A page of ClientUpdateMailHeaders: its headers, its version and bComplete; false when the call is another one or the page does not read.
static bool test_mailbox_page(const repl::RpcCall& call, std::vector<repl::MailHeader>& headers, uint64_t& version, bool& complete) {
    headers.clear();
    if (!call.rpc || std::string(call.rpc->name) != "ClientUpdateMailHeaders") return false;
    const repl::Value* field = repl::rpcf::value_of(*call.rpc, call.params, "Headers");
    const auto* list = field ? field->get_if<repl::ArrayValue>() : nullptr;
    if (field && !list) return false;
    if (list) {
        for (const repl::ElementState& element : list->elements) {
            const repl::Value* value = repl::rpcf::element_value(element, 1);
            const auto* header = value ? value->get_if<repl::MailHeader>() : nullptr;
            if (!header) return false;
            headers.push_back(*header);
        }
    }
    const repl::Value* version_value = repl::rpcf::value_of(*call.rpc, call.params, "Version");
    const auto* number = version_value ? version_value->get_if<uint64_t>() : nullptr;
    if (!number) return false;
    version = *number;
    const repl::Value* complete_value = repl::rpcf::value_of(*call.rpc, call.params, "bComplete");
    const auto* flag = complete_value ? complete_value->get_if<bool>() : nullptr;
    if (!flag) return false;
    complete = *flag;
    return true;
}

/// The SocialComponent's NumUnreadMail as the session holds it.
static bool test_mailbox_unread_state(const Harness& h, int32_t& out) {
    if (!h.s) return false;
    const repl::Subobject* social = h.s->bootstrap().player_controller().find(h.s->bootstrap().setup().guids.social);
    if (!social || !social->obj.schema) return false;
    const repl::Value* value = social->obj.state.get(*social->obj.schema, "NumUnreadMail");
    const auto* number = value ? value->get_if<int64_t>() : nullptr;
    out = number ? static_cast<int32_t>(*number) : 0;
    return true;
}

/// A session harness with the object rows injected, the mailbox and a quest object that is not a mailbox standing at the login spot.
struct test_mailbox_rig : Harness {
    uint64_t mailbox = 0, crate = 0;
    bool injected = false, world_ok = false, made = false, crate_made = false;
    std::string problem;
    test_mailbox_sink mail_sink;

    test_mailbox_rig() : Harness(cyb_record(), test_mailbox_options()) {
        fx.world->set_position_sink(&mail_sink);
        injected = test_mailbox_inject(fx.dir / "content.db");
        if (!injected) {
            problem = "the quest object rows do not load";
            return;
        }
        spawns::AreaConfig area;
        area.center_x = bs::kDefaultSpawn.x;
        area.center_y = bs::kDefaultSpawn.y;
        area.radius_cm = 1000.0;
        std::string area_error;
        fx.world->load_area(area, area_error);   // (the fixture has no population chain: the load stops after the objects, which are what this test needs)
        world_ok = fx.world->objects() != nullptr;
        if (!world_ok) {
            problem = "the world reads no quest object definitions: " + area_error;
            return;
        }
        spawns::GenericConfig config;
        config.prop_components.insert(test_mailbox_icd);
        config.prop_components.insert(test_mailbox_crate_icd);
        spawns::GenericNpcs generic(*fx.captured, nullptr, config);
        content::NpcAssetRow asset = test_mailbox_asset_row(test_mailbox_asset_guid, "Hu_Kae_N3_Mailbox_ADef", test_mailbox_path());
        content::NpcInstanceRow instance = test_mailbox_instance_row(test_mailbox_instance_guid, "N3_Mailbox_Spawn", 0, 0);
        spawns::Verdict why = spawns::Verdict::Generic;
        const spawns::OpenTemplate* open = generic.prop_template(asset, &why);
        if (!open) {
            problem = "the mailbox Blueprint is not made as a prop";
            return;
        }
        spawns::DataSpawn s;
        s.x = bs::kDefaultSpawn.x + 100.0;
        s.y = bs::kDefaultSpawn.y;
        s.z = bs::kDefaultSpawn.z;
        s.level = 1;
        s.asset = &asset;
        s.instance = &instance;
        s.open = open;
        s.seed = 0x9e3779b97f4a7c15ull;
        std::string error;
        auto w = spawns::make_data_actor(s, fx.minter(), kServerId, 0.0, nullptr, error, nullptr);
        if (!w) {
            problem = "the mailbox actor: " + error;
            return;
        }
        mailbox = w->id;
        if (!fx.world->registry().add(std::move(*w))) {
            problem = "the registry refuses the mailbox actor";
            return;
        }
        made = true;

        content::NpcAssetRow crate_asset = test_mailbox_asset_row(test_mailbox_crate_asset_guid, "CSQ_Crate_ADef", test_mailbox_crate_path());
        content::NpcInstanceRow crate_instance = test_mailbox_instance_row(test_mailbox_crate_instance_guid, "N3_Crate_Spawn", test_mailbox_crate_icd, test_mailbox_crate_pid);
        const spawns::OpenTemplate* crate_open = generic.prop_template(crate_asset, &why);
        if (!crate_open) {
            problem = "the crate Blueprint is not made as a prop";
            return;
        }
        spawns::DataSpawn cs;
        cs.x = bs::kDefaultSpawn.x - 100.0;
        cs.y = bs::kDefaultSpawn.y;
        cs.z = bs::kDefaultSpawn.z;
        cs.level = 1;
        cs.asset = &crate_asset;
        cs.instance = &crate_instance;
        cs.open = crate_open;
        cs.seed = 0x9e3779b97f4a7c16ull;
        auto cw = spawns::make_data_actor(cs, fx.minter(), kServerId, 0.0, nullptr, error, nullptr);
        if (!cw) {
            problem = "the crate actor: " + error;
            return;
        }
        crate = cw->id;
        if (!fx.world->registry().add(std::move(*cw))) {
            problem = "the registry refuses the crate actor";
            return;
        }
        crate_made = true;
    }
};

/// The RPC calls the client received on the pawn's channel since `from` (the mailbox's own answer, ClientOnDoInteract, is the pawn's).
static std::vector<repl::RpcCall> test_mailbox_pawn_calls_since(const Harness& h, size_t from) {
    std::vector<repl::RpcCall> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pawn_ch || h.got[i].open || h.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(h.got[i].content.data(), h.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t p = b.payload_pos;
            auto calls = repl::read_rpc_fields(class_named("PlayerPawn_C"), h.got[i].content.data(), b.payload_pos + b.payload_bits, p);
            if (calls) {
                for (auto& c : *calls) out.push_back(std::move(c));
            }
        }
    }
    return out;
}

// The mailbox's use opens the window with the mailbox's own base guid; a quest object that is not a mailbox opens none.
static void test_mailbox_open() {
    std::printf("the mailbox: its use opens the mail window with its own base guid\n");
    test_mailbox_rig h;
    CHECK(h.injected, "the fixture takes the mailbox's quest object rows");
    if (!h.injected) return;
    CHECK(h.world_ok, "the world reads the quest objects");
    if (!h.world_ok) {
        if (!h.problem.empty()) std::printf("    %s\n", h.problem.c_str());
        return;
    }
    CHECK(h.made, "the mailbox Blueprint is made as a prop");
    if (!h.made) {
        if (!h.problem.empty()) std::printf("    %s\n", h.problem.c_str());
        return;
    }
    CHECK(h.crate_made, "a quest object that is not a mailbox is made as a prop");
    if (!h.crate_made) {
        if (!h.problem.empty()) std::printf("    %s\n", h.problem.c_str());
        return;
    }
    h.start();
    h.play();
    h.run(milliseconds(1500));
    const world::WorldActor* a = h.fx.world->registry().find(h.mailbox);
    CHECK(a != nullptr && h.s->actor_view() && h.s->actor_view()->state_of(h.mailbox) == world::ClientState::Awake, "the mailbox is open on the client");
    if (!a) return;
    const repl::GuidValue base = test_mailbox_state_guid(*a);
    CHECK(base != repl::GuidValue{}, "the spawn wrote the mailbox's InteractableBaseGuid");
    CHECK(a->npc.interact_component != 0, "the mailbox has an interactable component");
    const size_t from = h.got.size();
    h.client_request(interact(a->npc.interact_component), h.pawn_ch);
    const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientOpenMailUI") == 1, "the use opens the mail window");
    CHECK(count_calls(test_mailbox_pawn_calls_since(h, from), "ClientOnDoInteract") == 1, "the quest object still answers the use");
    std::optional<repl::GuidValue> got;
    for (const repl::RpcCall& c : calls) {
        if (c.rpc && std::string(c.rpc->name) == "ClientOpenMailUI") {
            got = test_mailbox_call_guid(c);
            break;
        }
    }
    CHECK(got.has_value(), "the window call carries the mailbox's four guid words");
    if (got) CHECK(*got == base, "the window names the mailbox's InteractableBaseGuid, not another guid");
    const size_t again_from = h.got.size();
    h.client_request(interact(a->npc.interact_component), h.pawn_ch);
    CHECK(count_calls(pc_calls_since(h, again_from), "ClientOpenMailUI") == 1, "the same mailbox used again opens the window again");
    const world::WorldActor* c = h.fx.world->registry().find(h.crate);
    CHECK(c != nullptr, "the quest object that is not a mailbox stands");
    if (c) {
        const size_t crate_from = h.got.size();
        h.client_request(interact(c->npc.interact_component), h.pawn_ch);
        CHECK(count_calls(pc_calls_since(h, crate_from), "ClientOpenMailUI") == 0, "a quest object that is not a mailbox opens no mail window");
    }
}
AOC_REGISTER_TEST(test_mailbox_open);

// The mailbox's use sends the mails that wait for him to the window: three mails from the town's post, each with its own title and its unread
// mark, the last page complete; the same mailbox used again sends the list again.
static void test_mailbox_use_lists_the_waiting_mails() {
    std::printf("the mailbox: its use lists his waiting mails in the window\n");
    test_mailbox_rig h;
    CHECK(h.injected, "the fixture takes the mailbox's quest object rows");
    if (!h.injected) return;
    CHECK(h.world_ok, "the world reads the quest objects");
    if (!h.world_ok) return;
    CHECK(h.made, "the mailbox Blueprint is made as a prop");
    if (!h.made) return;
    const char* const titles[3] = {"A letter from the front", "The warehouse delivery", "A summons to the fair"};
    h.mail_sink.mails = {test_mailbox_mail(0x9001, titles[0]), test_mailbox_mail(0x9002, titles[1]), test_mailbox_mail(0x9003, titles[2])};
    h.start();
    h.play();
    h.run(milliseconds(1500));
    const world::WorldActor* a = h.fx.world->registry().find(h.mailbox);
    CHECK(a != nullptr && h.s->actor_view() && h.s->actor_view()->state_of(h.mailbox) == world::ClientState::Awake, "the mailbox is open on the client");
    if (!a) return;
    const size_t from = h.got.size();
    h.client_request(interact(a->npc.interact_component), h.pawn_ch);
    const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientOpenMailUI") == 1, "the use opens the mail window");
    CHECK(count_calls(calls, "ClientUpdateMailHeaders") == 1, "the window is sent one page of headers");
    std::vector<repl::MailHeader> headers;
    uint64_t version = 0;
    bool complete = false;
    const repl::RpcCall* page = nullptr;
    for (const repl::RpcCall& c : calls) {
        if (c.rpc && std::string(c.rpc->name) == "ClientUpdateMailHeaders") {
            page = &c;
            break;
        }
    }
    CHECK(page && test_mailbox_page(*page, headers, version, complete), "the header page reads back");
    CHECK(headers.size() == 3, "the page carries his three mails");
    CHECK(complete, "the last page is marked complete");
    if (headers.size() == 3) {
        size_t unread = 0;
        bool senders = true, names = true;
        for (size_t i = 0; i < 3; ++i) {
            senders = senders && headers[i].sender == "Lionhold Post";
            names = names && headers[i].subject == titles[i];
            unread += (headers[i].status & svc::kMailStatusRead) == 0 ? 1 : 0;
        }
        CHECK(senders, "each mail names the town's post as its sender");
        CHECK(names, "each mail carries its own title");
        CHECK(unread == 3, "the three headers are unread, so the window's count reads 3");
    }
    int32_t social_unread = -1;
    CHECK(test_mailbox_unread_state(h, social_unread) && social_unread == 3, "the social component's unread count is 3");
    const size_t again = h.got.size();
    h.client_request(interact(a->npc.interact_component), h.pawn_ch);
    const std::vector<repl::RpcCall> again_calls = pc_calls_since(h, again);
    CHECK(count_calls(again_calls, "ClientUpdateMailHeaders") == 1, "the same mailbox used again sends the list again");
    std::vector<repl::MailHeader> again_headers;
    bool again_complete = false;
    for (const repl::RpcCall& c : again_calls) {
        if (c.rpc && std::string(c.rpc->name) == "ClientUpdateMailHeaders") {
            CHECK(test_mailbox_page(c, again_headers, version, again_complete), "the page of the second use reads back");
            break;
        }
    }
    CHECK(again_headers.size() == 3 && again_complete, "the second use lists the same three mails, complete");
}
AOC_REGISTER_TEST(test_mailbox_use_lists_the_waiting_mails);

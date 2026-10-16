// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_session.cpp: worldd after NMT_Join (plan A5, E3)
//
//  What a fake client sees when the endpoint runs the native bootstrap, and what the
//  session answers.  The bootstrap's own bits are proven against the original server in
//  test_worldd_bootstrap; here the proof is that the session SENDS them: the same
//  character, content.db, login identity and session values go through an independent
//  PlayerBootstrap, and what the client receives (each chain whole: its export bunches,
//  its content) is compared with that sequence, on the channels the session picked, in
//  order.  The requests are the retail client's own bunches where the capture has them
//  (libs/aocnet/tests/capture_blocks.h: the possession bunch with ServerUpdateAccountCombat
//  Settings and ServerRequestClientLoad, ServerClientIsReadyToPhaseIn; capture_bootstrap.h:
//  four ServerMovePacked), else built from the class schemas.
//
//    identity       the NMT_Login of the e2e capture parses to its ?Name= and unique id; the
//                   control channel keeps them; a Login without them falls back, with a note
//    endpoint       content.db missing: the login is refused with kContentMissing before the
//                   ticket is redeemed; a character the bootstrap refuses is told
//                   kLoginFailed at Join; a full join: 16 chains on channels 3, 4, 5, 6,
//                   equal to the bootstrap's, cyb's name and Guid in the pawn, the login's
//                   name and unique id in the PlayerState; acks reach mark_exported
//    session        channels skip open and closing indexes; ServerClientIsReadyToPhaseIn
//                   -> the phase-in update, once; ServerMovePacked -> ClientMoveResponse
//                   Packed with the move's TimeStamp (checked against the legacy server's
//                   parse of the same bunch); the possession bunch -> the combat-settings
//                   echo, the four storage payloads, the group and the action bar;
//                   ServerRequestStorageData; unknown and malformed bunches are ignored
//    window         a client that stops acknowledging while it asks for its load again and
//                   again: the replies wait for the window, nothing is lost or reordered; a
//                   phase-in or echo that waits for the window arrives once, afterwards
//    acks           a lost packet's exports are not marked (and are once the resend is
//                   acknowledged); a datagram's acks are applied before its requests;
//                   nothing is tracked once everything is acknowledged
//    budgets        1,000 loads in a second cost the server a burst's worth, the first drop is
//                   logged once, a request after a quiet period is served; 1,000 possession
//                   acknowledgements give three log lines
//    endpoint       two players at once (one GameState GUID, PlayerId 1 and 2), a kicked
//                   session answers nothing more, a join that can never be sent ends with the
//                   Failure, an unsupported race gets its own text
//    login tail     NMT_Login's unique id in hostile and odd forms
//
//  spdlog is pointed at a stream so the log lines can be checked (and the ticket's absence).
// ============================================================================
#include "bootstrap/captured_replies.h"
#include "bootstrap/captured_setup.h"
#include "capture_blocks.h"
#include "capture_bootstrap.h"
#include "capture_packets.h"
#include "content_db_fixture.h"
#include "world_stats.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"
#include "protocol/wire/server_move_parser.h"
#include "session/player_session.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
// C.O.A / Cyb

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace wire = aoc::protocol::wire;
namespace fs = std::filesystem;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
using std::chrono::milliseconds;
using std::chrono::seconds;

static std::ostringstream g_log;
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
static const std::string kTicket = "9ddc8eac805cc2fc3e75df52b74613c8";  // the captured Initial's
static const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";   // the capture's login name, 50 characters
static const Clock::time_point t0{seconds(5000)};
static const Address kA{0x7F000001u, 50510};
static const Address kB{0x7F000001u, 50511};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;

static uint8_t g_counter = 1;
static void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_counter++;
}

// ---- the world: a small content.db, the allocator, the PlayerWorld ----------------------------------------------------------

struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;

    /// `real_db`: open that content.db instead of making the small one.
    explicit Fixture(bs::GameStateMode mode = bs::GameStateMode::Tier2, bool with_content = true, const std::string& real_db = "") {
        std::string problem = "no such file";
        if (with_content) {
            fs::path db = real_db;
            if (real_db.empty()) {
                dir = content_fixture::make_temp_dir("worldd_session_");
                db = dir / "content.db";
                if (!content_fixture::make_db(db)) std::exit(2);
            }
            std::string error;
            content = content::ContentDb::open(db.string(), error);
            if (!content) {
                std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
                std::exit(2);
            }
            problem.clear();
        }
        world = std::make_unique<PlayerWorld>(content ? &*content : nullptr, problem, mode, kServerId, guids);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        std::error_code ec;
        if (!dir.empty()) fs::remove_all(dir, ec);
    }
};

/// cyb: a level-1 Kaelar male Bard with the customization as the character store returns it, no saved position.
static svc::Character cyb_record() {
    svc::Character c;
    c.id = kCyb;
    c.account_id = 3;
    c.name = "cyb";
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    return c;
}

/// A second character (another id and name) for two-player scenarios.
static svc::Character second_record() {
    svc::Character c = cyb_record();
    c.id = "11112222333344445555666677778888";
    c.name = "dee";
    return c;
}

static bs::LoginIdentity identity() { return {kMachine, kMachine}; }

/// How often `text` occurs in `log`.
static size_t count_lines(const std::string& log, const char* text) {
    size_t n = 0;
    for (size_t at = log.find(text); at != std::string::npos; at = log.find(text, at + 1)) ++n;
    return n;
}

static double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

/// The bootstrap an independent builder makes for the nth player of a fresh worldd (same allocator, same inputs).
static std::unique_ptr<bs::PlayerBootstrap> expected_bootstrap(const content::ContentDb& content, const svc::Character& rec,
                                                               double world_time, bs::GameStateMode mode, int nth = 1,
                                                               repl::ActionbarPayload* bar_out = nullptr) {
    aoc::protocol::ServerGuidAllocator guids(kServerId, kRandomizer);
    PlayerWorld w(&content, "", mode, kServerId, guids);
    bs::SessionValues sv;
    for (int i = 0; i < nth; ++i) sv = w.new_session(world_time);
    auto built = bs::build_player_setup(rec, content, identity(), sv, w.stat_context());  // the world's context: it carries the talent catalog the session's join uses
    if (!built.setup) {
        std::printf("  [FAIL] the expected setup does not build: %s\n", built.error.c_str());
        std::exit(2);
    }
    if (bar_out) bar_out->slots = built.action_bar.wire_slots();   // the character's own bar, as the session sends it
    auto pb = std::make_unique<bs::PlayerBootstrap>(std::move(*built.setup), mode);
    if (!pb->ok()) {
        std::printf("  [FAIL] the expected bootstrap does not build: %s\n", pb->error().c_str());
        std::exit(2);
    }
    return pb;
}

// ---- comparing and decoding ----------------------------------------------------------------------------------------------------
// [Cyb - C | O | A]

static bool same_bits(const BunchWriter& a, const BunchWriter& b) {
    return a.bit_pos() == b.bit_pos() && capture_blocks::same_bit_range(a.data(), b.data(), a.bit_pos(), nullptr);
}

/// "" when `got[first ...]` are exactly the Sends of `want`, on `channels` (indexed by role), else the first difference.
static std::string compare_sends(const std::vector<fake::Delivered>& got, size_t first, const std::vector<bs::Send>& want,
                                 const std::array<uint32_t, 4>& channels) {
    if (got.size() < first + want.size()) return "only " + std::to_string(got.size() - std::min(first, got.size())) + " chains arrived";
    for (size_t i = 0; i < want.size(); ++i) {
        const fake::Delivered& d = got[first + i];
        const bs::Send& s = want[i];
        const std::string at = "chain " + std::to_string(i) + " (" + s.what + "): ";
        if (d.channel != channels[static_cast<size_t>(s.role)]) return at + "channel " + std::to_string(d.channel);
        if (d.open != s.opens_channel) return at + "open flag";
        if (d.reliable != s.reliable()) return at + "reliability";
        if (d.exports.size() != s.emission.exports.size()) return at + std::to_string(d.exports.size()) + " export bunches";
        for (size_t e = 0; e < d.exports.size(); ++e) {
            if (!same_bits(d.exports[e], s.emission.exports[e])) return at + "export bunch " + std::to_string(e);
        }
        if (!same_bits(d.content, s.emission.payload)) return at + "content";
    }
    return "";
}

static const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

/// The names of the RPCs a delivered chain calls (one content block each).
static std::vector<std::string> rpc_names(const fake::Delivered& d, const schema::ClassSchema& cls) {
    std::vector<std::string> out;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return {"<unreadable>"};
    for (const repl::BlockView& v : *blocks) {
        size_t p = v.payload_pos;
        const auto calls = repl::read_rpc_fields(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
        if (!calls) {
            out.push_back("<not an rpc>");
            continue;
        }
        for (const repl::RpcCall& c : *calls) out.push_back(c.rpc->name);
    }
    return out;
}

struct NewActor {
    FIntrepidNetworkGUID actor, archetype;
    size_t end = 0;   // the bit after SerializeNewActor (and a PlayerController's two fields)
    bool ok = false;
};

/// UPackageMapClient::SerializeNewActor (test_worldd_bootstrap.cpp read_new_actor): the actor and archetype GUIDs.
static NewActor read_new_actor(const BunchWriter& w, bool player_controller) {
    NewActor n;
    wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    auto read_guid = [&] {
        FIntrepidNetworkGUID g;
        const uint64_t lo = r.read_uint32(), hi = r.read_uint32();
        g.ObjectId = lo | (hi << 32);
        g.ServerId = r.read_uint32();
        g.Randomizer = r.read_uint32();
        return g;
    };
    n.actor = read_guid();
    n.archetype = read_guid();
    read_guid();   // the level
    auto vector = [&] {
        if (!r.read_bit()) return true;
        if (!r.read_bit()) return false;
        return repl::read_quantized_vector(r).has_value();
    };
    if (!vector()) return n;
    if (r.read_bit() && !repl::read_struct("FRotator", r)) return n;
    if (!vector() || !vector()) return n;
    if (player_controller) {
        r.read_uint8();
        r.read_uint32();
    }
    n.ok = !r.overflowed();
    n.end = r.pos();
    return n;
}
// ~ C:Y:B - C O A ~

/// The RepLayout block of the object `object` (the actor when `actor`) in a delivered open or update, read with `cls`.
static std::optional<repl::RepState> block_of(const fake::Delivered& d, size_t start, bool actor, const FIntrepidNetworkGUID& object,
                                              const schema::ClassSchema& cls) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), start);
    if (!blocks) return std::nullopt;
    for (const repl::BlockView& v : *blocks) {
        if (!v.header.has_rep_layout) continue;
        if (actor ? !v.header.is_actor : (v.header.is_actor || !(v.header.object == object))) continue;
        size_t p = v.payload_pos;
        return repl::read_rep_block(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
    }
    return std::nullopt;
}

static FIntrepidNetworkGUID guid(uint64_t id) {
    FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = kServerId;
    g.Randomizer = kRandomizer;
    return g;
}

/// A client bunch payload: one RPC of the actor's class (a content block of the client's form).
static BunchWriter client_rpc(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params = {}) {
    const schema::RpcSchema* rpc = cls.rpc(name);
    BunchWriter out;
    if (!rpc) {
        std::printf("  [FAIL] no RPC %s\n", name);
        return out;
    }
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    repl::BlockHeader header;   // the actor's own: no RepLayout block
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

static BunchWriter hex_bits(const capture_blocks::Bits& b) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, b.bits);
    return w;
}
static BunchWriter hex_bits(const capture_bootstrap::Bits& b) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, b.bits);
    return w;
}

/// The TimeStamp of a captured ServerMovePacked bunch as the legacy server reads it (parse_server_move_no_base: the float
/// after "flags", "roll" and a bit, which is the packed move data's first 32 bits).
static std::optional<float> legacy_time_stamp(const BunchWriter& bunch) {
    wire::FServerMoveNoBase move;
    if (!wire::parse_server_move_no_base(bunch.data(), bunch.byte_size(), 0, bunch.bit_pos(), move)) return std::nullopt;
    return move.timestamp;
}

/// The float of a delivered ClientMoveResponsePacked: the content block's field header, 10 header bits, then the TimeStamp.
static std::optional<float> response_time_stamp(const fake::Delivered& d) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks || blocks->size() != 1) return std::nullopt;
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    size_t pos = (*blocks)[0].payload_pos;
    const auto h = repl::read_field_header(pawn, d.content.data(), pos + (*blocks)[0].payload_bits, pos);
    if (!h || !pawn.field(h->field) || std::string(pawn.field(h->field)->name) != "ClientMoveResponsePacked") return std::nullopt;
    wire::PacketReader r(d.content.data(), d.content.byte_size(), pos + h->payload_bits);
    r.set_pos(pos);
    const uint64_t header = r.read_bits(10);
    const uint32_t bits = static_cast<uint32_t>(r.read_bits(32));
    if (r.overflowed() || header != 645 || h->payload_bits != 42) return std::nullopt;
    return std::bit_cast<float>(bits);
}

// ---- the endpoint rig (test_worldd_endpoint.cpp's, with the player world) ----------------------------------------------

struct FakeLogins : LoginQueue {
    std::vector<std::pair<uint64_t, LoginRequest>> submitted;
    void submit(uint64_t id, LoginRequest r) override { submitted.emplace_back(id, std::move(r)); }
};

struct Rig {
    CookieSigner signer{counting_random};
    FakeLogins logins;
    std::vector<std::pair<Address, std::vector<uint8_t>>> sent;
    Endpoint ep;

    explicit Rig(EndpointConfig cfg)
        : ep(std::move(cfg), signer, logins, [this](const Address& a, const std::vector<uint8_t>& p) { sent.emplace_back(a, p); },
             counting_random, t0) {}

    std::vector<std::vector<uint8_t>> take(const Address& a) {
        std::vector<std::vector<uint8_t>> out;
        std::vector<std::pair<Address, std::vector<uint8_t>>> rest;
        for (auto& [to, p] : sent) {
            if (to == a) out.push_back(p);
            else rest.emplace_back(to, p);
        }
        sent.swap(rest);
        return out;
    }
};

struct Joined {
    std::optional<conn::HandshakePacket> challenge;
    std::optional<fake::Client> client;
    uint64_t session = 0;
    std::vector<fake::Delivered> got;   // every actor chain the client has been given
};
// [C | O | A + C:Y:B]

static Joined handshake(Rig& rig, const Address& addr, Clock::time_point now) {
    Joined j;
    rig.take(addr);
    const auto initial = capture::bytes(capture::kInitial);
    rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
    auto out = rig.take(addr);
    if (out.size() != 1) return j;
    j.challenge = conn::parse_handshake(out[0].data(), out[0].size());
    conn::HandshakePacket resp = *j.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    const auto bytes = conn::build_handshake(resp);
    rig.ep.on_datagram(addr, bytes.data(), bytes.size(), now);
    out = rig.take(addr);
    if (out.size() != 1) return j;
    const auto seqs = conn::initial_sequences(j.challenge->cookie);
    j.client.emplace(conn::outer_header_of(*j.challenge), j.challenge->ext->custom_field(), seqs.server, seqs.client);
    j.session = rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
    return j;
}

static std::vector<fake::Received> drain(Rig& rig, Joined& j, const Address& addr) {
    std::vector<fake::Received> got;
    for (const auto& p : rig.take(addr)) {
        got.push_back(j.client->receive(p));
        for (const auto& d : got.back().delivered) j.got.push_back(d);
    }
    return got;
}

static std::vector<fake::Received> exchange(Rig& rig, Joined& j, const Address& addr, std::vector<fake::Bunch> bunches,
                                            Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    return drain(rig, j, addr);
}

static const conn::ControlMessage* find_message(const std::vector<fake::Received>& got, conn::Nmt type) {
    for (const auto& r : got)
        for (const auto& m : r.messages)
            if (m.type == type) return &m;
    return nullptr;
}

static bool closed_by_server(const std::vector<fake::Received>& got) {
    for (const auto& r : got)
        if (r.closed) return true;
    return false;
}

static LoginResult cyb_login() {
    LoginResult r;
    r.accepted = true;
    r.character = cyb_record();
    return r;
}

/// Hello, the accepted login, Login (the retail client's: ?Name= and the unique id, both `machine`) -> Welcome.
static void log_in_as(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now, const std::string& machine) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(j.session, r);
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login(machine, machine)}, now + milliseconds(2));
}
static void log_in(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    log_in_as(rig, j, addr, rec, now, kMachine);
}

/// Join from `j` (Netspeed and Join in one packet), the endpoint ticked, the client given what came.
static void join_now(Rig& rig, Joined& j, const Address& addr, Clock::time_point now) {
    const auto p = j.client->packet({j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)});
    rig.ep.on_datagram(addr, p.data(), p.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}

// ============================================================================================================================
//  identity
// ============================================================================================================================

static void test_identity() {
    std::printf("the login identity\n");
    // The e2e capture's own NMT_Login, a real packet of the retail client
    const auto bytes = capture::bytes(capture::kClientLogin);
    const auto p = conn::read_data_packet(bytes.data(), bytes.size(), wire::Direction::ClientToServer);
    std::optional<conn::ControlMessage> m;
    if (p) {
        const auto bunches = conn::packet_bunches(bytes.data(), bytes.size(), *p);
        if (bunches.size() == 1) m = conn::parse_control_message(bytes.data(), bytes.size(), bunches[0].data_start_bit, bunches[0].bunch_data_bits);
    }
    CHECK(m && m->type == conn::Nmt::Login && m->has_unique_id && m->platform_name == "NULL",
          "the captured NMT_Login parses with its unique id (flags 8) and platform name 'NULL'");
    CHECK(m && m->request_url == "?Name=" + m->unique_id && m->unique_id.rfind("DESKTOP-EXAMPL2-", 0) == 0 && m->unique_id.size() == 48,
          "its unique id is the ?Name= value, the 48 characters the transcript names (section 2.3)");
// (C.O.A | C | Y | B)

    ControlChannel c(WorldInfo{}, "X");
    conn::ControlMessage hello;
    hello.type = conn::Nmt::Hello;
    c.on_message(hello);
    CHECK(!c.login_message(), "no Login yet: nothing kept");
    if (m) c.on_message(*m);
    CHECK(c.login_message() && c.login_message()->has_unique_id && c.login_message()->unique_id == m->unique_id &&
              c.login_message()->request_url == m->request_url,
          "the control channel keeps the Login's URL and unique id");

    CHECK(url_option("?Name=cyb", "Name") == "cyb" && url_option("?Name=cyb?Team=1", "name") == "cyb" && url_option("Map?Team=1?Name=a b", "Name") == "a b" &&
              url_option("?Team=1", "Name").empty() && url_option("", "Name").empty() && url_option("?Name", "Name").empty() && url_option("?Name=", "Name").empty(),
          "url_option: the value up to the next '?', the key without case, nothing when absent");

    std::vector<std::string> notes;
    const svc::Character rec = cyb_record();
    auto id = make_login_identity(LoginMessage{"?Name=" + kMachine, true, kMachine}, rec, &notes);
    CHECK(id.name_option == kMachine && id.unique_id == kMachine && notes.empty(), "a full Login: the name option and the unique id as sent, no note");
    id = make_login_identity(LoginMessage{"?Name=" + kMachine, false, ""}, rec, &notes);
    CHECK(id.name_option == kMachine && id.unique_id == kMachine && notes.size() == 1, "no unique id: the name option stands in, with a note");
    notes.clear();
    id = make_login_identity(LoginMessage{"?Team=1", true, "ID"}, rec, &notes);
    CHECK(id.name_option == "cyb" && id.unique_id == "ID" && notes.size() == 1, "no ?Name=: the character's name stands in, with a note");
    notes.clear();
    id = make_login_identity(std::nullopt, rec, &notes);
    CHECK(id.name_option == "cyb" && id.unique_id == "cyb" && notes.size() == 2, "no Login at all: both fall back to the character's name");
}

// ============================================================================================================================
//  through the endpoint
// ============================================================================================================================

static void test_content_missing() {
    std::printf("content.db missing\n");
    Fixture fx(bs::GameStateMode::Tier2, false);
    CHECK(!fx.world->ready() && !fx.world->content_problem().empty(), "the world is not ready and says why");
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    g_log.str("");
    Joined j = handshake(rig, kA, t0);
    CHECK(j.client && rig.ep.session_count() == 1 && rig.logins.submitted.empty(),
          "the handshake completes but no login is submitted: the join ticket is not redeemed for a refusal");
    bool closed = false;
    auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
    const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
    closed = closed_by_server(got);
    CHECK(failure && failure->error == login_text::kContentMissing && closed, "Hello gets NMT_Failure with the content.db text, and the channel closes");
    CHECK(std::string(login_text::kContentMissing).find("content.db") != std::string::npos, "the text names content.db for the operator");
    CHECK(!rig.ep.session_of(kCyb), "the character is not logged in");
    CHECK(g_log.str().find("worldd has no content.db (no such file)") != std::string::npos && g_log.str().find(kTicket) == std::string::npos,
          "the log says why, without the ticket");
}

/// A content.db built before plan B2 has no item or item_equip_slot table: ContentDb::open fails (main.cpp then runs worldd with no content and the
/// reason), and every login is refused exactly as with a missing file; the reason the log gives says to rebuild the file.
static void test_content_built_before_b2() {
    std::printf("a content.db built before plan B2\n");
    const fs::path dir = content_fixture::make_temp_dir("worldd_session_old_");
    content_fixture::DbOptions old_schema;
    old_schema.item_tables = false;
    const fs::path path = dir / "content.db";
    if (!content_fixture::make_db(path, old_schema)) std::exit(2);
    std::string error;
    std::optional<content::ContentDb> content = content::ContentDb::open(path.string(), error);
    CHECK(!content && error.find("does not have the tables this worldd reads") != std::string::npos && error.find("item") != std::string::npos &&
              error.find("rebuild it with this checkout's contentc") != std::string::npos,
          "the file does not open: it lacks the item tables, and the error says to rebuild it with contentc");
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    // as main.cpp builds it when the file did not open
    PlayerWorld world(content ? &*content : nullptr, error, bs::GameStateMode::Tier2, kServerId, guids);
    CHECK(!world.ready() && world.content_problem() == error, "the world is not ready, and its reason is the one open() gave");
    EndpointConfig cfg;
    cfg.players = &world;
    Rig rig(cfg);
    g_log.str("");
    Joined j = handshake(rig, kA, t0);
    CHECK(j.client && rig.ep.session_count() == 1 && rig.logins.submitted.empty(), "the handshake completes but no login is submitted: the join ticket is not redeemed");
    auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
    const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
    CHECK(failure && failure->error == login_text::kContentMissing && closed_by_server(got), "Hello gets NMT_Failure with the content.db text, and the channel closes: every login is refused");
    CHECK(!rig.ep.session_of(kCyb), "the character is not logged in");
    // a second player is refused the same way: the refusal is not a one-off
    Joined k = handshake(rig, kB, t0 + milliseconds(20));
    auto got_k = exchange(rig, k, kB, {k.client->hello()}, t0 + milliseconds(30));
    const conn::ControlMessage* failure_k = find_message(got_k, conn::Nmt::Failure);
    CHECK(failure_k && failure_k->error == login_text::kContentMissing && rig.logins.submitted.empty(), "and so is the next client");
    CHECK(g_log.str().find("worldd has no content.db (") != std::string::npos && g_log.str().find("rebuild it with this checkout's contentc") != std::string::npos &&
              g_log.str().find(kTicket) == std::string::npos,
          "the log says why and how to rebuild the file, without the ticket");
    content.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}
// ~ C-O-A :: C | Y | B ~

/// Plan B3: a content.db whose stat program is missing, damaged, of another graph, or made by an older contentc is no content.db to serve players
/// with: the world is not ready, its reason says what and how to rebuild, and every login is refused as with a missing file.
static void test_stat_program_problems() {
    std::printf("a content.db with no usable stat program (plan B3)\n");
    const fs::path dir = content_fixture::make_temp_dir("worldd_session_statprog_");
    std::vector<uint8_t> damaged = stats_fixture::main_bundle();
    damaged[damaged.size() / 2] ^= 0x01;   // a byte of the body: the crc says so
    const std::vector<uint8_t> none;
    const std::vector<uint8_t>& main_bytes = stats_fixture::main_bundle();
    const std::vector<uint8_t>& tiny = stats_fixture::corpus().bundles.at("tiny");   // a valid bundle of another graph
    struct Case {
        const char* what;
        const std::vector<uint8_t>* bytes;
        const char* want;
        int64_t crc_xor = 0;
    };
    const Case cases[] = {
        {"a damaged bundle (one flipped byte)", &damaged, "does not verify"},
        {"no row in stat_program", &none, "no stat program"},
        {"a valid bundle of another graph (the tiny one of the tests)", &tiny, "not one this worldd can run"},
        {"a row whose crc32 is not the bundle header's", &main_bytes, "bundle's header says", 0x5a5a},
    };
    int n = 0;
    for (const Case& c : cases) {
        content_fixture::DbOptions o;
        o.stat_program_bytes = c.bytes;
        o.stat_program_crc_xor = c.crc_xor;
        const fs::path path = dir / (std::string("c") + std::to_string(n++) + ".db");
        if (!content_fixture::make_db(path, o)) std::exit(2);
        std::string error;
        std::optional<content::ContentDb> content = content::ContentDb::open(path.string(), error);
        CHECK(content.has_value(), (std::string(c.what) + ": the file opens (its tables are there)").c_str());
        if (!content) continue;
        aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
        PlayerWorld world(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
        const auto hint_count = [&] {
            size_t k = 0;
            for (size_t at = world.content_problem().find("go run ./cmd/contentc"); at != std::string::npos; at = world.content_problem().find("go run ./cmd/contentc", at + 1)) ++k;
            return k;
        };
        CHECK(hint_count() == 1, (std::string(c.what) + ": the way out is said once, not twice").c_str());
        CHECK(!world.ready() && world.content_problem().find(c.want) != std::string::npos &&
                  world.content_problem().find("go run ./cmd/contentc") != std::string::npos && world.stat_bundle() == nullptr && world.content() == nullptr,
              (std::string(c.what) + ": the world is not ready, and says '" + c.want + "' and how to rebuild").c_str());
        if (n == 1) {
            EndpointConfig cfg;
            cfg.players = &world;
            Rig rig(cfg);
            Joined j = handshake(rig, kA, t0);
            auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
            const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
            CHECK(failure && failure->error == login_text::kContentMissing && rig.logins.submitted.empty(), "a login is refused with the content.db text, before the ticket is redeemed");
        }
    }
    {
        content_fixture::DbOptions ok;
        const fs::path path = dir / "ok.db";
        if (!content_fixture::make_db(path, ok)) std::exit(2);
        std::string error;
        std::optional<content::ContentDb> content = content::ContentDb::open(path.string(), error);
        aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
        PlayerWorld world(content ? &*content : nullptr, "", bs::GameStateMode::Tier2, kServerId, guids);
        const bs::StatContext ctx = world.stat_context();
        CHECK(world.ready() && world.stat_bundle() && ctx.bundle == world.stat_bundle() && ctx.gear && ctx.rounding == stats::Rounding::HalfUp &&
                  world.stat_bundle()->classes.size() == 10,
              "(control) the fixture's real stat program makes a ready world with a bundle of ten classes (the eight player classes and the two NPC profiles) and the item data");
        content.reset();
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static const std::array<uint32_t, 4> kChannels = {3, 4, 5, 6};   // PlayerController, GameState, pawn, PlayerState

static void test_join_through_endpoint() {
    std::printf("a join through the endpoint\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    g_log.str("");
    Joined j = handshake(rig, kA, t0);
    log_in(rig, j, kA, cyb_record(), t0);
    const Clock::time_point t_join = t0 + milliseconds(40);
    auto got = exchange(rig, j, kA, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)}, t_join);
    CHECK(!find_message(got, conn::Nmt::Failure) && rig.ep.session_count() == 1, "Netspeed and Join are accepted");

    auto want_pb = expected_bootstrap(*fx.content, cyb_record(), world_time_at(t_join), bs::GameStateMode::Tier2);
    const bs::PlayerSetup setup = want_pb->setup();
    auto seq = want_pb->join_sequence();
    CHECK(seq && seq->size() == 16, "the bootstrap's join sequence has 16 chains");
    if (!seq) return;
    CHECK(j.got.size() == 16, "the client receives 16 chains after Join");
    const std::string diff = compare_sends(j.got, 0, *seq, kChannels);
    CHECK(diff.empty(), "they are the bootstrap's sequence, bit for bit and in order, on channels 3, 4, 5, 6");
    if (!diff.empty()) std::printf("    %s\n", diff.c_str());
    const std::array<uint32_t, 15> order = {3, 3, 3, 3, 4, 3, 5, 5, 5, 5, 5, 3, 3, 5, 6};
    bool channels_ok = j.got.size() == 16;
    for (size_t i = 0; channels_ok && i < order.size(); ++i) channels_ok = j.got[i].channel == order[i];
    CHECK(channels_ok, "the order across channels: PlayerController x4, GameState, PlayerController, pawn x5, PlayerController x2, pawn, PlayerState");
    CHECK(j.got.size() == 16 && j.got[0].open && j.got[4].open && j.got[6].open && j.got[14].open &&
              !j.got[1].open && !j.got[5].open && !j.got[13].open,
          "the four actors' first chains open their channels and no other does");

    // what the client can read in them: the right character, the login's identity, minted GUIDs
    if (j.got.size() < 15) return;
    const NewActor pc = read_new_actor(j.got[0].content, true), gs = read_new_actor(j.got[4].content, false),
                   pawn = read_new_actor(j.got[6].content, false), ps = read_new_actor(j.got[14].content, false);
    CHECK(pc.ok && gs.ok && pawn.ok && ps.ok, "the four opens' SerializeNewActor read");
    CHECK(gs.actor == guid(0x1000000) && ps.actor == guid(0x1000002) && pc.actor == guid(0x1000004) && pawn.actor == guid(0x1000008),
          "GUIDs are minted under worldd's ServerId: the GameState's first, then the PlayerState, PlayerController and pawn (even ObjectIds)");
    CHECK(pc.archetype.ObjectId == bs::pc_archetype_export().front().guid.ObjectId &&
              gs.archetype.ObjectId == bs::game_state_archetype_export().front().guid.ObjectId &&
              pawn.archetype.ObjectId == bs::pawn_archetype_export().front().guid.ObjectId &&
              ps.archetype.ObjectId == bs::player_state_archetype_export().front().guid.ObjectId,
          "each open names its class: AoCPlayerControllerBP_C, AoCGameStateBP_C, PlayerPawn_C, AoCPlayerStateBP_C");
    const schema::ClassSchema& base_cls = *gen::find_component_schema("BaseCharacterInfo");
    const auto base = block_of(j.got[6], pawn.end, false, setup.guids.base_character_info, base_cls);
    const repl::Value* name = base ? base->get(base_cls, "CharacterName") : nullptr;
    const repl::Value* a = base ? base->get(base_cls, "CharacterGuid.A") : nullptr;
    const repl::Value* d4 = base ? base->get(base_cls, "CharacterGuid.D") : nullptr;
    CHECK(name && *name == repl::Value::str("cyb") && a && *a == repl::Value::i64(static_cast<int32_t>(0xbbec5c86u)) && d4 &&
              *d4 == repl::Value::i64(static_cast<int32_t>(0xfa22e7d0u)),
          "the pawn's BaseCharacterInfo: CharacterName 'cyb' and the record's Guid bbec5c86 ... fa22e7d0");
    const schema::ClassSchema& ps_cls = class_named("AoCPlayerStateBP_C");
    const auto ps_state = block_of(j.got[14], ps.end, true, {}, ps_cls);
    const repl::Value* uid = ps_state ? ps_state->get(ps_cls, "UniqueId") : nullptr;
    const repl::Value* pname = ps_state ? ps_state->get(ps_cls, "PlayerNamePrivate") : nullptr;
    const auto* u = uid ? uid->get_if<repl::UniqueNetId>() : nullptr;
    CHECK(u && u->flags == 8 && u->id == kMachine && pname && *pname == repl::Value::str(kMachine.substr(0, 20)),
          "the PlayerState: the login's unique id (flags 8) and the first 20 characters of its ?Name=, not the character's name");
    const repl::Value* pid = ps_state ? ps_state->get(ps_cls, "PlayerId") : nullptr;
    CHECK(pid && *pid == repl::Value::i64(1), "PlayerId 1, the first player");

    // acknowledgements: the client's next packet acks all of it; a later reply names the Backpack without its export
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const size_t before = j.got.size();
    got = exchange(rig, j, kA, {}, t_join + milliseconds(100));   // the ack-only packet
    got = exchange(rig, j, kA, {j.client->actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)}, t_join + milliseconds(200));
    CHECK(j.got.size() == before + 6, "ServerRequestClientLoad is answered with six chains on the PlayerController's channel");
    if (j.got.size() == before + 6) {
        const auto names = rpc_names(j.got[before], pc_cls);
        CHECK(j.got[before].channel == 3 && names.size() == 1 && names[0] == "ClientReceiveStoragePayload" && j.got[before].exports.empty(),
              "the Backpack payload: the component's export was acknowledged with the join, so it goes without its path");
    }

    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    CHECK(log.find("[session] cyb: joining as ServerId 60, PlayerId 1") != std::string::npos &&
              log.find("sent PlayerController open on channel 3 (2 parts)") != std::string::npos &&
              log.find("sent GameState open (tier 2) on channel 4") != std::string::npos &&
              log.find("sent PlayerState open on channel 6") != std::string::npos,
          "the log has the join and each step sent: name, channel, parts");
    CHECK(log.find("default spawn") != std::string::npos, "and the setup's notes (the default spawn)");
    CHECK(log.find(kTicket) == std::string::npos, "the ticket is not in the log");
}

static void test_setup_refused_at_join() {
    std::printf("a character the bootstrap refuses\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    g_log.str("");
    Joined j = handshake(rig, kA, t0);
    svc::Character rec = cyb_record();
    rec.race_id = 8;   // Tulnar: no player appearance in the design data, never created (character_setup.h): the setup is refused
    log_in(rig, j, kA, rec, t0);
    auto got = exchange(rig, j, kA, {j.client->nmt(conn::Nmt::Join)}, t0 + milliseconds(40));
    const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
    CHECK(failure && failure->error == login_text::kRaceUnsupported && closed_by_server(got),
          "Join of a Tulnar (a race that is never created): NMT_Failure with the race's own text and the channel closes");
    CHECK(j.got.empty(), "nothing was spawned");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("cannot join the world") != std::string::npos && g_log.str().find("race") != std::string::npos,
          "the log has the reason: the race id");

    // control: a setup refused for any other reason keeps the generic text
    Rig other(cfg);
    Joined k = handshake(other, kB, t0);
    svc::Character broken = cyb_record();
    broken.appearance_json = "[";
    log_in(other, k, kB, broken, t0);
    got = exchange(other, k, kB, {k.client->nmt(conn::Nmt::Join)}, t0 + milliseconds(40));
    failure = find_message(got, conn::Nmt::Failure);
    CHECK(failure && failure->error == login_text::kLoginFailed && closed_by_server(got), "(control: broken appearance JSON is refused with the generic 'Login failed')");
}

// ============================================================================================================================
//  the session on a bare connection
// ============================================================================================================================
// ~ C_Y_B - CoA ~

static const conn::OuterHeader kOuter{0x500c7696u, 0, 5};
static const conn::CustomField kCf{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
static constexpr uint16_t kServerSeq = 14265, kClientSeq = 15870;

/// A Connection, a fake client on its other end, and a PlayerSession (joined by hand, as the endpoint does).
struct Harness {
    Fixture fx;
    Connection conn{ConnectionConfig{kOuter, kCf, kServerSeq, kClientSeq}, t0};
    fake::Client client{kOuter, kCf, kServerSeq, kClientSeq};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::vector<std::vector<uint8_t>> held;   // server packets the client has not been given (it is not acknowledging)
    bool client_receives = true;
    size_t packets = 0, bytes = 0;   // what the server sent
    std::string error;

    explicit Harness(bs::GameStateMode mode = bs::GameStateMode::Tier2, const std::string& real_db = "") : fx(mode, true, real_db) {}

    bool join(const svc::Character& rec = cyb_record()) {
        s = PlayerSession::join(conn, *fx.world, rec, identity(), world_time_at(t), error);
        return s != nullptr;
    }
    void take(const fake::Received& r) {
        for (const auto& d : r.delivered) got.push_back(d);
    }
    void flush() {
        for (auto& pkt : conn.flush(t)) {
            ++packets;
            bytes += pkt.size();
            if (client_receives) take(client.receive(pkt));
            else held.push_back(std::move(pkt));
        }
    }
    void give_held() {
        for (const auto& pkt : held) take(client.receive(pkt));
        held.clear();
    }
    /// The client sends `bunches` in one packet (which acknowledges what it has received so far), at `t`; the acks
    /// are taken first, as the endpoint does.
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        const std::vector<InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const InBunch& b : in) s->on_bunch(conn, b, t);
    }
    /// `n` copies of the RPC block `one` on `channel`, as few reliable bunches as fit a packet, `apart` between
    /// packets, the server ticking and flushing after each.  Returns the packets sent.
    size_t client_repeats(uint32_t channel, const BunchWriter& one, size_t n, milliseconds apart = milliseconds(1)) {
        const size_t fit = std::max<size_t>(1, (Connection::kMaxBunchBits - 200) / one.bit_pos());
        size_t packets = 0;
        for (size_t left = n; left > 0; ++packets) {
            const size_t count = std::min(fit, left);
            BunchWriter many;
            for (size_t i = 0; i < count; ++i) many.write_bit_range(one.data(), 0, one.bit_pos());
            t += apart;
            client_sends({client.actor(channel, many, true)});
            s->tick(conn);
            flush();
            left -= count;
        }
        return packets;
    }
    /// 20 ms: the server ticks and sends, the client receives and acknowledges.
    void step() {
        t += milliseconds(20);
        s->tick(conn);
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    /// Steps until nothing is pending and a step brings nothing new.
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = got.size();
            step();
            const bool idle = got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0;
            quiet = idle ? quiet + 1 : 0;
        }
    }
    /// The chains the client was given on `channel` from index `from`.
    std::vector<const fake::Delivered*> on_channel(uint32_t channel, size_t from = 0) const {
        std::vector<const fake::Delivered*> out;
        for (size_t i = from; i < got.size(); ++i)
            if (got[i].channel == channel) out.push_back(&got[i]);
        return out;
    }
};

static void test_channels() {
    std::printf("actor channels\n");
    Harness h;
    CHECK(h.join(), "a session joins a fresh connection");
    CHECK(h.s->channel_of(bs::Role::PlayerController) == 3 && h.s->channel_of(bs::Role::GameState) == 4 &&
              h.s->channel_of(bs::Role::Pawn) == 5 && h.s->channel_of(bs::Role::PlayerState) == 6,
          "it takes the lowest free indexes from 3: 3, 4, 5, 6");

    // channels 3 and 5 open, 4 closing (its close not acknowledged), and 6 free: the session skips them
    Harness busy;
    Chain open;
    open.open = true;
    open.payload.write_bits(1, 8);
    for (uint32_t ch : {3u, 4u, 5u}) {
        open.channel = ch;
        busy.conn.send_chain(open);
    }
    busy.conn.close_channel(4);
    CHECK(busy.conn.channel_open(3) && busy.conn.channel_closing(4) && busy.conn.channel_open(5), "(channels 3 and 5 are open, 4 is closing)");
    CHECK(busy.join(), "a session joins a connection that has actor channels");
    CHECK(busy.s->channel_of(bs::Role::PlayerController) == 6 && busy.s->channel_of(bs::Role::GameState) == 7 &&
              busy.s->channel_of(bs::Role::Pawn) == 8 && busy.s->channel_of(bs::Role::PlayerState) == 9,
          "it skips the open and the closing ones: 6, 7, 8, 9");
    busy.settle();
    CHECK(busy.on_channel(6).size() == 8 && busy.on_channel(7).size() == 1 && busy.on_channel(8).size() == 6 && busy.on_channel(9).size() == 1,
          "and sends on them: eight chains on the PlayerController's (the XPTracker's last), one on the GameState's, six on the pawn's, one on the PlayerState's");
    bool filler = false;
    for (const auto& d : busy.got) filler = filler || d.channel < 6;
    CHECK(filler, "(the filler's own chains on 3, 4 and 5 reached the client as well)");
}

static void test_join_and_acks() {
    std::printf("the join and its acknowledgements\n");
    Harness h;
    CHECK(h.join() && !h.s->failed(), "join");
    CHECK(h.s->join_pending() == 0, "the whole join went to the connection at once (the windows had room)");
    h.step();
    const size_t join_packets = h.packets, join_bytes = h.bytes;
    h.settle();
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    auto seq = pb->join_sequence();
    CHECK(seq && h.got.size() == 16 && compare_sends(h.got, 0, *seq, kChannels).empty(), "16 chains, the bootstrap's sequence in order");
    CHECK(!h.s->failed() && h.s->replies_pending() == 0, "nothing pending, nothing failed");
    CHECK(h.s->exports_tracked() == 0, "every export part of the join had its final ack event: no chain is tracked any more");
    std::printf("    the join of cyb (Tier 2) is %zu packets, %zu bytes (the original's: 64 packets, 51 KB in its first 174 ms)\n", join_packets, join_bytes);
// C.O.A + C.Y.B

    // the client acknowledged every packet: the PlayerController's replies leave out the exports of what it has
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    h.settle();
    const auto pc = h.on_channel(3, before);
    CHECK(pc.size() == 6, "ServerRequestClientLoad: six chains");
    std::vector<std::string> names;
    for (const auto* d : pc) names.push_back(rpc_names(*d, pc_cls).empty() ? "" : rpc_names(*d, pc_cls)[0]);
    const std::vector<std::string> expect = {"ClientReceiveStoragePayload", "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
                                             "ClientReceiveStoragePayload", "ClientUpdateGroup", "ClientReceiveActionbarSlotPayload"};
    CHECK(names == expect, "four storage payloads (Backpack, Equipment, QuestStorage, the resource storage), the group and the action bar, in that order");
    bool no_exports = true;
    for (const auto* d : pc) no_exports = no_exports && d->exports.empty();
    CHECK(no_exports, "all acknowledged: no export bunch in any of them (the join's acks reached mark_exported)");
    CHECK(h.s->exports_tracked() == 0, "(and the replies, which carried no export, left nothing tracked either)");
    const auto* backpack = pc.size() > 0 ? pc[0] : nullptr;
    repl::ActionbarPayload bar;
    auto pb_acked = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2, 1, &bar);
    auto seq2 = pb_acked->join_sequence();
    if (seq2) for (const bs::Send& snd : *seq2) for (const auto& bunch : snd.emission.exported_by_bunch) pb_acked->mark_exported(bunch);
    const auto want_backpack = pb_acked->storage_payload(bs::StorageKind::Backpack, bs::Role::PlayerController);
    CHECK(backpack && want_backpack && same_bits(backpack->content, want_backpack->emission.payload) && want_backpack->emission.exports.empty(),
          "the Backpack payload equals the bootstrap's after every export of the join is acknowledged");

    // The group is the original's block; the action bar is the character's own (plan D1: the weapon bank and the class's skills, built from
    // the class's talent tree), which is the bootstrap's action_bar() of the setup's bar, not the captured Bard's five slots
    const auto want_bar = pb_acked->action_bar(bar);
    CHECK(pc.size() == 6 && pc[4]->content.bit_pos() == bs::captured::kEmptyGroupBlock.bits, "the group is the original's 527 bits (pkt#606)");
    CHECK(pc.size() == 6 && want_bar && same_bits(pc[5]->content, want_bar->emission.payload), "the action bar is the setup's own bar: the weapon bank and the class's skills");

    // control: a client that has acknowledged nothing is sent the exports
    Harness fresh;
    fresh.client_receives = false;
    fresh.join();
    fresh.step();
    fresh.client_sends({fresh.client.actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    fresh.give_held();
    fresh.client_receives = true;
    fresh.step();
    const auto fpc = fresh.on_channel(3);
    CHECK(!fpc.empty() && want_bar && same_bits(fpc.back()->content, want_bar->emission.payload) && fpc.size() >= 13, "(control: the same request from a client that acknowledged nothing yet)");
    size_t with_export = 0;
    for (size_t i = fpc.size() >= 6 ? fpc.size() - 6 : 0; i < fpc.size(); ++i) with_export += fpc[i]->exports.empty() ? 0 : 1;
    CHECK(with_export == 3, "its Backpack, Equipment and QuestStorage payloads carry the component's export (the resource storage has none)");
}

static void test_phase_in_and_moves() {
    std::printf("phase-in and moves\n");
    Harness h;
    h.join();
    h.settle();
    const schema::ClassSchema& pawn_cls = class_named("PlayerPawn_C");
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);

    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    const auto phase = h.on_channel(pawn_ch, before);
    CHECK(phase.size() == 1 && phase[0]->reliable && phase[0]->exports.empty(), "ServerClientIsReadyToPhaseIn: one reliable update on the pawn's channel, no exports");
    if (phase.size() == 1) {
        const auto blocks = repl::read_content_blocks(phase[0]->content.data(), phase[0]->content.bit_pos(), 0);
        CHECK(blocks && blocks->size() == 2, "the actor's block and one subobject's");
        if (blocks && blocks->size() == 2) {
            const repl::BlockView& actor = (*blocks)[0];
            const repl::BlockView& sub = (*blocks)[1];
            size_t p = actor.payload_pos;
            const auto a = repl::read_rep_block(pawn_cls, phase[0]->content.data(), actor.payload_pos + actor.payload_bits, p);
            const schema::ClassSchema& info = *gen::find_component_schema("BaseCharacterInfo");
            size_t q = sub.payload_pos;
            const auto b = repl::read_rep_block(info, phase[0]->content.data(), sub.payload_pos + sub.payload_bits, q);
            const repl::Value* hidden = a ? a->get(pawn_cls, "bHidden") : nullptr;
            const repl::Value* phased = b ? b->get(info, "bIsPhasedOut") : nullptr;
            const repl::Value* ready = b ? b->get(info, "bClientIsReadyToPhaseIn") : nullptr;
            CHECK(a && b && actor.header.is_actor && sub.header.object == h.s->bootstrap().setup().guids.base_character_info && hidden &&
                      *hidden == repl::Value::boolean(false) && phased && *phased == repl::Value::boolean(false) && ready &&
                      *ready == repl::Value::boolean(true),
                  "bHidden 0, BaseCharacterInfo.bIsPhasedOut 0 and bClientIsReadyToPhaseIn 1 (the original's pkt#2523)");
        }
    }
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    CHECK(h.on_channel(pawn_ch, before).size() == 1, "a second request is not answered again: once");

    // moves: three of the capture's, and the largest
    const std::array<const capture_bootstrap::Bits*, 4> moves = {&capture_bootstrap::kClientMovePacked1, &capture_bootstrap::kClientMovePacked2,
                                                                 &capture_bootstrap::kClientMovePacked3, &capture_bootstrap::kClientMovePacked4};
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    const size_t moves_from = h.got.size();
    std::vector<float> stamps;
    bool all_legacy = true;
    for (const auto* m : moves) {
        const BunchWriter bits = hex_bits(*m);
        const auto legacy = legacy_time_stamp(bits);
        all_legacy = all_legacy && legacy.has_value();
        stamps.push_back(legacy.value_or(-1.0f));
        h.client_sends({h.client.actor(pawn_ch, bits, false)});
    }
    h.step();
    CHECK(all_legacy && stamps[0] > 0.8f && stamps[0] < 0.82f && stamps[0] < stamps[1] && stamps[1] < stamps[2],
          "(the captured moves' TimeStamps read by the legacy parser: 0.8125 s and rising)");
    const auto acks = h.on_channel(pawn_ch, moves_from);
    CHECK(acks.size() == 4, "each ServerMovePacked is answered");
    bool unreliable = true, stamps_ok = true, bits_ok = true;
    for (size_t i = 0; i < acks.size() && i < stamps.size(); ++i) {
        unreliable = unreliable && !acks[i]->reliable && acks[i]->exports.empty() && acks[i]->content.bit_pos() == 68;
        const auto ts = response_time_stamp(*acks[i]);
        stamps_ok = stamps_ok && ts && *ts == stamps[i];
        const auto want = pb->good_move_ack(stamps[i]);
        bits_ok = bits_ok && want && same_bits(acks[i]->content, want->emission.payload);
    }
    CHECK(unreliable, "unreliable, 68 bits, on the pawn's channel");
    CHECK(stamps_ok, "ClientMoveResponsePacked carries the move's own TimeStamp (the legacy server's parse of the same bunch), also the 533-bit bunch's");
    CHECK(bits_ok, "equal to the bootstrap's good_move_ack, which equals the original's 68 bits (test_worldd_bootstrap)");
    CHECK(h.s->moves_acknowledged() == 4, "four moves acknowledged");
    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    CHECK(log.find("the client is ready to phase in") != std::string::npos && log.find("sent phase-in on channel 5") != std::string::npos &&
              log.find("the first move is acknowledged (TimeStamp 0.81252") != std::string::npos,
          "the log has the phase-in and the first move acknowledged (once)");
    size_t first_move_lines = 0;
    for (size_t at = log.find("the first move is acknowledged"); at != std::string::npos; at = log.find("the first move is acknowledged", at + 1)) ++first_move_lines;
    CHECK(first_move_lines == 1, "the move acknowledgement is logged once, not per move");

    // a move with a damaged parameter is not acknowledged
    BunchWriter damaged = hex_bits(capture_bootstrap::kClientMovePacked1);
    BunchWriter cut;   // the same bunch with DataBits' packed integer changed: the parameter no longer ends where the field says
    const auto bytes = capture_blocks::bytes_of(capture_bootstrap::kClientMovePacked1.hex);
    std::vector<uint8_t> mangled(bytes.begin(), bytes.end());
    mangled[6] ^= 0x04;   // inside the "send" bit / SerializeIntPacked(DataBits) bits of the parameter
    cut.write_bit_range(mangled.data(), 0, damaged.bit_pos());
    const size_t acked = h.s->moves_acknowledged();
    h.client_sends({h.client.actor(pawn_ch, cut, false)});
    h.step();
    CHECK(h.s->moves_acknowledged() == acked, "a ServerMovePacked whose parameter does not read as the capture's is not acknowledged");
}

static void test_possession_bunch_and_requests() {
    std::printf("the possession bunch, storage requests, the unknown and the malformed\n");
    Harness h;
    h.join();
    h.settle();
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::ClassSchema& pawn_cls = class_named("PlayerPawn_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);

    const size_t before = h.got.size();
    const BunchWriter possession = hex_bits(capture_blocks::kClientBunchPossession);
    h.client_sends({h.client.actor(pc_ch, possession, true)});
    h.settle();
    const auto pc = h.on_channel(pc_ch, before);
    CHECK(pc.size() == 7, "the retail client's possession bunch (72 RPCs): seven chains back: the combat-settings echo and the load's six");
    if (pc.size() != 7) return;
    std::vector<std::string> names;
    for (const auto* d : pc) {
        const auto n = rpc_names(*d, pc_cls);
        names.push_back(n.empty() ? "(property block)" : n[0]);
    }
    CHECK(names[0] == "<not an rpc>" || names[0] == "(property block)" || names[0] == "<unreadable>", "the first is a property block, not an RPC: the echo");
    const std::vector<std::string> rest = {"ClientReceiveStoragePayload", "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
                                           "ClientReceiveStoragePayload", "ClientUpdateGroup", "ClientReceiveActionbarSlotPayload"};
    CHECK(std::vector<std::string>(names.begin() + 1, names.end()) == rest, "then the four storage payloads, the group and the action bar");
// ~ C-O-A & C.Y.B ~

    // the echo: the nine values of pkt#620 as the request's own; eight equal the original's, PartyVFX is the client's
    const auto blocks = repl::read_content_blocks(pc[0]->content.data(), pc[0]->content.bit_pos(), 0);
    std::optional<repl::RepState> echo;
    if (blocks && blocks->size() == 1) {
        size_t p = (*blocks)[0].payload_pos;
        echo = repl::read_rep_block(pc_cls, pc[0]->content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, p);
    }
    const BunchWriter original = hex_bits(capture_bootstrap::kCombatEcho);
    const auto ob = repl::read_content_blocks(original.data(), original.bit_pos(), 0);
    std::optional<repl::RepState> orig;
    if (ob && ob->size() == 1) {
        size_t p = (*ob)[0].payload_pos;
        orig = repl::read_rep_block(pc_cls, original.data(), (*ob)[0].payload_pos + (*ob)[0].payload_bits, p);
    }
    CHECK(echo && orig && echo->values().size() == 9 && pc[0]->reliable && pc[0]->exports.empty(), "the echo is one reliable PlayerController block of nine values");
    if (echo && orig) {
        int equal = 0, party_vfx_differs = 0;
        for (const std::string_view path : kCombatEchoPaths) {
            const repl::Value* mine = echo->get(pc_cls, path);
            const repl::Value* theirs = orig->get(pc_cls, path);
            if (mine && theirs && *mine == *theirs) ++equal;
            else if (path.find("PartyVFX") != std::string_view::npos && mine && theirs) ++party_vfx_differs;
        }
        CHECK(equal == 8 && party_vfx_differs == 1,
              "eight of the nine values equal the original's (pkt#620); PartyVFX is the client's own (0 in this client's request, 1 in the original's reply)");
    }
    const auto parsed = repl::read_client_bunch(pc_cls, nullptr, possession.data(), possession.bit_pos());
    std::map<std::string, int> seen;
    if (parsed) for (const auto& r : parsed->rpcs) ++seen[r.call.rpc->name];
    CHECK(parsed && parsed->rpcs.size() == 72 && seen["ServerUpdateAccountCombatSettings"] == 1 && seen["ServerAcknowledgePossession"] == 1 &&
              seen["ServerRequestClientLoad"] == 1 && seen["NotifyAntiCheatState"] == 1 && seen["ServerRequestNodeSummary"] == 34 &&
              seen["ServerRequestCurrentSiegeSummary"] == 34,
          "(the request has 72 RPCs, as the capture says; worldd answers two of them, the node and siege summaries and the client-state notice are ignored)");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("acknowledged possession of another object, not of its pawn") != std::string::npos,
          "the captured acknowledgement names the original session's pawn, not this one's: logged as such");
    repl::RpcParams mine;
    repl::set_rpc_param(*pc_cls.rpc("ServerAcknowledgePossession"), mine, "P", repl::Value(h.s->bootstrap().setup().guids.pawn));
    h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerAcknowledgePossession", mine), true)});
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("the client acknowledged possession of its pawn") != std::string::npos, "the acknowledgement of this session's pawn is logged");

    // ServerRequestStorageData for the resource storage: the pawn's payload (pkt#773); for an unknown container: nothing
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const size_t b2 = h.got.size();
    repl::RpcParams params;
    repl::set_rpc_param(*pawn_cls.rpc("ServerRequestStorageData"), params, "StorageContainer", repl::Value(g.resource_storage[0]));
    h.client_sends({h.client.actor(pawn_ch, client_rpc(pawn_cls, "ServerRequestStorageData", params), true)});
    h.settle();
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    const auto want = pb->storage_payload(bs::StorageKind::Resource0, bs::Role::Pawn);
    const auto reply = h.on_channel(pawn_ch, b2);
    CHECK(reply.size() == 1 && want && same_bits(reply[0]->content, want->emission.payload) && reply[0]->exports.empty(),
          "ServerRequestStorageData(resource storage): the pawn's empty resource payload (pkt#773), no export");
    repl::RpcParams other;
    repl::set_rpc_param(*pawn_cls.rpc("ServerRequestStorageData"), other, "StorageContainer", repl::Value(guid(0x7777777)));
    const size_t b3 = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, client_rpc(pawn_cls, "ServerRequestStorageData", other), true)});
    h.settle();
    CHECK(h.got.size() == b3, "a container worldd does not know gets no reply");

    // an RPC worldd does not answer, one on an unknown channel, and bits that are no bunch
    const size_t b4 = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientSetActiveTarget), true)});
    h.client_sends({h.client.actor(40, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    BunchWriter garbage;
    for (int i = 0; i < 12; ++i) garbage.write_bits(0xFFFFFFFFu, 32);
    h.client_sends({h.client.actor(pc_ch, garbage, true)});
    BunchWriter truncated;
    truncated.write_bit_range(possession.data(), 0, 300);   // cut in the middle of a block
    h.client_sends({h.client.actor(pc_ch, truncated, true)});
    h.settle();
    CHECK(h.got.size() == b4 && !h.s->failed(), "ServerSetActiveTarget, a bunch on a channel that is not the player's, garbage and a cut bunch: ignored, nothing sent");
    h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    h.settle();
    CHECK(h.got.size() == b4 + 6, "and the session still answers afterwards");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("does not read: ignored") != std::string::npos, "the malformed bunches are logged");
}
// [C_Y_B ~ CoA]

/// Raw reliable bunches on channel 3 that the client gets and ignores: they take up the window of the channel the
/// PlayerController is about to use (Connection::send queues them without opening anything).
static void clog_channel(Harness& h, uint32_t channel, size_t bunches) {
    conn::BunchHeader header;
    header.channel = channel;
    header.reliable = true;
    header.name_index = Connection::kActorChannelName;
    BunchWriter filler;
    filler.write_bits(0xA5, 8);
    for (size_t i = 0; i < bunches; ++i) h.conn.send(header, filler);
}

static void test_join_waits_for_the_window() {
    std::printf("a join that does not fit the window\n");
    Harness h;
    h.client_receives = false;
    clog_channel(h, 3, 248);   // 8 bunches of room on channel 3: the PlayerController's chains need more than that in all
    CHECK(h.conn.unacked_reliable(3) == 248, "(248 reliable bunches in flight on channel 3)");
    CHECK(h.join() && !h.s->failed(), "the session joins");
    const size_t pending = h.s->join_pending();
    CHECK(pending > 0 && pending < 15, "part of the join is on the wire and the rest waits for the window: a chain stays at the head of the queue");
    std::printf("    %zu of the 16 chains were waiting\n", pending);
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    // the client asks for its load while the join is incomplete: the replies come after the join, never before
    h.client_sends({h.client.actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    CHECK(h.s->replies_pending() == 6 && h.s->join_pending() == pending, "the request's six reply steps wait behind the join");
    h.s->tick(h.conn);
    CHECK(h.s->join_pending() == pending, "a tick with the window still full changes nothing");
    h.flush();

    h.client_receives = true;
    h.give_held();
    h.settle();
    CHECK(h.s->join_pending() == 0 && h.s->replies_pending() == 0 && !h.s->failed(), "once the client acknowledges, the join completes and then the replies");
    std::vector<fake::Delivered> mine;   // without the filler's one-byte bunches
    for (const auto& d : h.got)
        if (d.content.bit_pos() != 8) mine.push_back(d);
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    auto seq = pb->join_sequence();
    CHECK(seq && mine.size() == 16 + 6 && compare_sends(mine, 0, *seq, kChannels).empty(),
          "the client has the join's 16 chains in the bootstrap's order, equal to it, and then the six replies");
    bool replies_last = mine.size() == 22;
    for (size_t i = 16; replies_last && i < 22; ++i) replies_last = mine[i].channel == 3 && !rpc_names(mine[i], pc_cls).empty();
    CHECK(replies_last, "the six replies are the last six, on the PlayerController's channel");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("the window of channel 3 is full") != std::string::npos, "the wait is logged (once)");
}

static void test_window_full() {
    std::printf("a client that stops acknowledging\n");
    Harness h;
    h.client_receives = false;
    CHECK(h.join(), "join");
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    // The client asks for its load over and over and acknowledges nothing: it never sees a packet.  The requests come
    // kLoadInterval apart, what the load's budget allows: the window, not the budget, is what this test fills.
    size_t most_unacked = h.conn.unacked_reliable(pc_ch);
    for (int i = 0; i < 40; ++i) {
        h.t += PlayerSession::kLoadInterval;
        h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
        h.s->tick(h.conn);
        h.flush();
        most_unacked = std::max(most_unacked, h.conn.unacked_reliable(pc_ch));
    }
    CHECK(most_unacked <= Connection::kMaxUnackedReliable, "the channel never holds more than 256 unacknowledged reliable bunches");
    CHECK(h.s->replies_pending() > 0 && !h.s->failed(), "replies wait for the window (they are not dropped, and the session does not fail)");
    CHECK(h.s->replies_pending() <= PlayerSession::kMaxPendingReplies, "and at most kMaxPendingReplies steps wait: the rest of the requests are ignored");
    const size_t waiting = h.s->replies_pending();
    std::printf("    %zu reply steps were waiting, %zu bunches unacknowledged at most\n", waiting, most_unacked);
    CHECK(h.s->requests_dropped() == 0, "no request was over its budget: the waiting is the window's doing");

    // now the client receives everything and acknowledges
    h.client_receives = true;
    h.give_held();
    h.settle();
    CHECK(h.s->replies_pending() == 0 && h.s->join_pending() == 0, "once it acknowledges, every waiting step goes out");
    const auto pc = h.on_channel(pc_ch);
    CHECK(pc.size() >= 8 + 6 * 10 && (pc.size() - 8) % 6 == 0, "the PlayerController's chains are the join's eight and whole loads of six after them");
    if (pc.size() < 8) return;
    const std::vector<std::string> cycle = {"ClientReceiveStoragePayload", "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
                                            "ClientReceiveStoragePayload", "ClientUpdateGroup", "ClientReceiveActionbarSlotPayload"};
    bool in_order = true;
    for (size_t i = 8; i < pc.size(); ++i) {
        const auto n = rpc_names(*pc[i], pc_cls);
        in_order = in_order && n.size() == 1 && n[0] == cycle[(i - 8) % 6];
    }
    CHECK(in_order, "every load arrived whole and in order: storage x4, group, action bar, again and again");
    std::printf("    %zu loads delivered of 40 asked\n", (pc.size() - 8) / 6);
    // the connection's reliable sequence on the channel is the client's view: the join's chains still equal the bootstrap's
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    auto seq = pb->join_sequence();
    std::vector<fake::Delivered> first_16(h.got.begin(), h.got.begin() + std::min<size_t>(16, h.got.size()));
    CHECK(seq && compare_sends(first_16, 0, *seq, kChannels).empty(), "the join itself was delivered in order, first, on its four channels");
}

// ============================================================================================================================
//  replies that wait for the window arrive once
// ============================================================================================================================

static void test_phase_in_waits() {
    std::printf("the phase-in waits for the pawn channel's window\n");
    Harness h;
    h.join();
    h.settle();
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
    // Fill the pawn channel's window with raw reliable bunches the client will receive but not acknowledge yet.
    h.client_receives = false;
    const size_t room = Connection::kMaxUnackedReliable - h.conn.unacked_reliable(pawn_ch);
    clog_channel(h, pawn_ch, room);
    CHECK(h.conn.unacked_reliable(pawn_ch) == Connection::kMaxUnackedReliable, "(the pawn channel's window is full)");
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.s->tick(h.conn);
    h.flush();
    CHECK(h.s->replies_pending() == 1, "the phase-in waits");
    h.client_receives = true;
    h.give_held();
    h.settle();
    // the filler's bunches are 8 bits; the phase-in is the one that is not
    size_t real = 0;
    for (const auto* d : h.on_channel(pawn_ch, before)) if (d->content.bit_pos() != 8) ++real;
    CHECK(real == 1, "after the acknowledgement the phase-in update arrives, exactly once (the checkpoint was restored while it waited)");
    CHECK(h.s->replies_pending() == 0, "nothing pending");
}
// [CoA x C-Y-B]

static void test_echo_waits() {
    std::printf("the combat-settings echo waits for the PlayerController channel's window\n");
    Harness h;
    h.join();
    h.settle();
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    h.client_receives = false;
    const size_t room = Connection::kMaxUnackedReliable - h.conn.unacked_reliable(pc_ch);
    clog_channel(h, pc_ch, room);
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, hex_bits(capture_blocks::kClientBunchPossession), true)});
    h.s->tick(h.conn);
    h.flush();
    CHECK(h.s->replies_pending() == 7, "the echo and the load's six wait");
    h.client_receives = true;
    h.give_held();
    h.settle();
    size_t real = 0;
    for (const auto* d : h.on_channel(pc_ch, before)) if (d->content.bit_pos() != 8) ++real;
    CHECK(real == 7, "after the acknowledgement: the echo and the six replies, each once (the PlayerController shadow was restored too)");
}

// ============================================================================================================================
//  lost export packets
// ============================================================================================================================

/// Joins with the client not receiving, and gives the client every held packet but those carrying an export bunch of a
/// pawn-channel chain (the Backpack's export is in several).  Returns how many were dropped.
static size_t join_dropping_pawn_exports(Harness& h) {
    h.client_receives = false;
    h.join();
    h.step();   // the server's packets are held
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
    std::set<size_t> drops;
    for (size_t i = 0; i < h.held.size(); ++i) {
        const auto p = conn::read_data_packet(h.held[i].data(), h.held[i].size(), wire::Direction::ServerToClient);
        if (!p) continue;
        for (const auto& b : conn::packet_bunches(h.held[i].data(), h.held[i].size(), *p))
            if (b.channel == pawn_ch && b.has_package_map_exports) drops.insert(i);
    }
    std::vector<std::vector<uint8_t>> keep;
    for (size_t i = 0; i < h.held.size(); ++i) if (!drops.count(i)) keep.push_back(h.held[i]);
    h.held.clear();
    for (const auto& pkt : keep) h.take(h.client.receive(pkt));
    return drops.size();
}

static size_t big_payloads(const std::vector<fake::Delivered>& chains, const schema::ClassSchema& pc_cls, size_t* with_export) {
    size_t big = 0;
    *with_export = 0;
    for (const auto& d : chains) {
        const auto n = rpc_names(d, pc_cls);
        if (n.empty() || n[0] != "ClientReceiveStoragePayload" || d.content.bit_pos() <= 10000) continue;
        ++big;
        if (!d.exports.empty()) ++*with_export;
    }
    return big;
}

static void test_lost_export() {
    std::printf("a lost packet's exports are not marked exported\n");
    Harness h;
    const size_t dropped = join_dropping_pawn_exports(h);
    CHECK(dropped > 0, "(the packets with the pawn's export bunches were dropped)");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    // The client's packet acknowledges everything but the dropped ones and asks for its load at once.
    h.t += milliseconds(20);
    h.client_sends({});   // the acknowledgement alone: the server learns of the loss (events taken)
    h.t += milliseconds(20);
    h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    h.s->tick(h.conn);
    // do not deliver the resend yet: read the reply's chains straight from the connection
    std::vector<fake::Delivered> pc_chains;
    for (const auto& pkt : h.conn.flush(h.t)) {
        auto r = h.client.receive(pkt);
        for (const auto& d : r.delivered) if (d.channel == pc_ch) pc_chains.push_back(d);
    }
    size_t with_export = 0;
    const size_t big = big_payloads(pc_chains, pc_cls, &with_export);
    CHECK(big == 3 && with_export == 3, "the Backpack, Equipment and QuestStorage payloads still carry their exports: the pawn's export packet was lost");
}
// CoA : C.Y.B

static void test_lost_then_resent() {
    std::printf("a lost export packet is resent and delivered: later replies leave its exports out\n");
    Harness h;
    join_dropping_pawn_exports(h);
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    h.client_receives = true;
    h.settle();   // the client acknowledges (with the hole), the server resends, the client acknowledges the resend
    CHECK(h.s->exports_tracked() == 0, "once the resend is acknowledged nothing is tracked");
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    h.settle();
    std::vector<fake::Delivered> chains;
    for (const auto* d : h.on_channel(pc_ch, before)) chains.push_back(*d);
    size_t with_export = 0;
    const size_t big = big_payloads(chains, pc_cls, &with_export);
    CHECK(big == 3 && with_export == 0, "once the resent packet is acknowledged its exports are marked: the replies carry none");
}

// ============================================================================================================================
//  budgets and the log lines the client can cause
// ============================================================================================================================

static void test_request_budget() {
    std::printf("the request budgets\n");
    RequestBudget b(4, seconds(2));
    int taken = 0;
    for (int i = 0; i < 10; ++i) taken += b.take(t0) ? 1 : 0;
    CHECK(taken == 4, "a fresh budget of 4 gives 4 at once and no more");
    CHECK(!b.take(t0 + seconds(1)) && b.take(t0 + seconds(2)) && !b.take(t0 + seconds(2)), "one more after the interval, not before");
    CHECK(!b.take(t0 + seconds(3) + milliseconds(999)) && b.take(t0 + seconds(4)), "the remainder of an interval carries over");
    taken = 0;
    for (int i = 0; i < 10; ++i) taken += b.take(t0 + seconds(100)) ? 1 : 0;
    CHECK(taken == 4, "a long quiet period refills to the burst, not beyond");

    Harness h;
    h.join();
    h.settle();
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::ClassSchema& pawn_cls = class_named("PlayerPawn_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
    const BunchWriter load = client_rpc(pc_cls, "ServerRequestClientLoad");
    const size_t bytes_before = h.bytes, chains_before = h.got.size();
    g_log.str("");
    // 1,000 loads within one simulated second, as few packets as they fit, each answered as far as the budget goes
    const size_t packets = h.client_repeats(pc_ch, load, 1000);
    h.settle();
    const size_t sent = h.bytes - bytes_before;
    const size_t loads = h.on_channel(pc_ch, chains_before).size() / 6;
    std::printf("    1,000 loads in %zu packets within a second: %zu answered, %zu bytes sent, %zu requests dropped\n", packets, loads, sent,
                h.s->requests_dropped());
    CHECK(!h.s->failed() && !h.conn.failed(), "(the session and the connection took the flood)");
    CHECK(loads == PlayerSession::kLoadBurst && h.s->requests_dropped() == 1000 - PlayerSession::kLoadBurst,
          "the burst's worth are answered, the rest dropped");
    CHECK(sent < 300 * 1024, "under a few hundred KB went out for the 1,000 requests");
    spdlog::default_logger()->flush();
    CHECK(count_lines(g_log.str(), "faster than its budget") == 1, "the first drop is logged, once");
    // after a quiet period a request is served again
    h.t += seconds(5);
    size_t at = h.got.size();
    h.client_sends({h.client.actor(pc_ch, load, true)});
    h.settle();
    CHECK(h.on_channel(pc_ch, at).size() == 6, "a load after a quiet period is answered");

    // the storage payloads have their own, larger budget
    repl::RpcParams params;
    repl::set_rpc_param(*pawn_cls.rpc("ServerRequestStorageData"), params, "StorageContainer", repl::Value(h.s->bootstrap().setup().guids.resource_storage[0]));
    const BunchWriter storage = client_rpc(pawn_cls, "ServerRequestStorageData", params);
    at = h.got.size();
    const size_t dropped_before = h.s->requests_dropped();
    h.client_repeats(pawn_ch, storage, 40);
    h.settle();
    CHECK(h.on_channel(pawn_ch, at).size() == PlayerSession::kStorageBurst && h.s->requests_dropped() == dropped_before + 40 - PlayerSession::kStorageBurst,
          "40 storage requests within a second: the storage burst's worth are answered");
    h.t += PlayerSession::kStorageInterval * 3;
    at = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, storage, true)});
    h.client_sends({h.client.actor(pawn_ch, storage, true)});
    h.client_sends({h.client.actor(pawn_ch, storage, true)});
    h.client_sends({h.client.actor(pawn_ch, storage, true)});
    h.settle();
    CHECK(h.on_channel(pawn_ch, at).size() == 3, "three intervals later: three more, not four");
}
// [C O A x C-Y-B]

static void test_client_driven_log_lines() {
    std::printf("the log lines the client can cause\n");
    Harness h;
    h.join();
    h.settle();
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    repl::RpcParams mine;
    repl::set_rpc_param(*pc_cls.rpc("ServerAcknowledgePossession"), mine, "P", repl::Value(h.s->bootstrap().setup().guids.pawn));
    const BunchWriter ack = client_rpc(pc_cls, "ServerAcknowledgePossession", mine);
    g_log.str("");
    h.client_repeats(pc_ch, ack, 1000);
    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    CHECK(count_lines(log, "acknowledged possession") == PlayerSession::kLoggedPerKind,
          "1,000 possession acknowledgements: 'acknowledged possession' is logged kLoggedPerKind (3) times, the rest at debug in other words");
    CHECK(count_lines(log, "possession ack #") == 1000 - PlayerSession::kLoggedPerKind, "(the debug lines count the rest)");
    // the reply chains' "sent" lines: the first three at info, the rest at debug (same words, so all are in this debug log)
    const BunchWriter load = client_rpc(pc_cls, "ServerRequestClientLoad");
    g_log.str("");
    h.client_sends({h.client.actor(pc_ch, load, true)});
    h.settle();
    spdlog::default_logger()->flush();
    const std::string lines = g_log.str();
    CHECK(count_lines(lines, "[info] [session] cyb: sent ") == PlayerSession::kLoggedPerKind && count_lines(lines, "[debug] [session] cyb: sent ") == 3,
          "a load's six reply chains: three 'sent' lines at info, three at debug");
}

// ============================================================================================================================
//  through the endpoint: two players, a kicked session, a join that cannot be sent, acks before requests
// ============================================================================================================================

static void test_two_players() {
    std::printf("two players at once\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    Joined b = handshake(rig, kB, t0 + milliseconds(1));
    log_in_as(rig, a, kA, cyb_record(), t0, kMachine);
    log_in_as(rig, b, kB, second_record(), t0 + milliseconds(3), "DESKTOP-OTHER-0000000000000000000000000000");
    CHECK(a.session != b.session, "two sessions");
    // both Join in the same tick
    const auto pa = a.client->packet({a.client->netspeed(200000), a.client->nmt(conn::Nmt::Join)});
    const auto pb = b.client->packet({b.client->netspeed(200000), b.client->nmt(conn::Nmt::Join)});
    rig.ep.on_datagram(kA, pa.data(), pa.size(), t0 + milliseconds(40));
    rig.ep.on_datagram(kB, pb.data(), pb.size(), t0 + milliseconds(40));
    rig.ep.tick(t0 + milliseconds(40));
    drain(rig, a, kA);
    drain(rig, b, kB);
    CHECK(a.got.size() == 16 && b.got.size() == 16, "each client has 16 chains");
    if (a.got.size() < 15 || b.got.size() < 15) return;
    const NewActor gsa = read_new_actor(a.got[4].content, false), gsb = read_new_actor(b.got[4].content, false);
    const NewActor pca = read_new_actor(a.got[0].content, true), pcb = read_new_actor(b.got[0].content, true);
    const NewActor pwa = read_new_actor(a.got[6].content, false), pwb = read_new_actor(b.got[6].content, false);
    const NewActor psa = read_new_actor(a.got[14].content, false), psb = read_new_actor(b.got[14].content, false);
    CHECK(gsa.actor == gsb.actor, "the GameState GUID is the same for both players");
    CHECK(!(pca.actor == pcb.actor) && !(pwa.actor == pwb.actor) && !(psa.actor == psb.actor), "PlayerController, pawn and PlayerState GUIDs differ");
    CHECK(!(psa.actor == pcb.actor) && !(pca.actor == pwb.actor), "no GUID of one is a GUID of the other");
    const schema::ClassSchema& ps_cls = class_named("AoCPlayerStateBP_C");
    const auto sa = block_of(a.got[14], psa.end, true, {}, ps_cls);
    const auto sb = block_of(b.got[14], psb.end, true, {}, ps_cls);
    const repl::Value* ida = sa ? sa->get(ps_cls, "PlayerId") : nullptr;
    const repl::Value* idb = sb ? sb->get(ps_cls, "PlayerId") : nullptr;
    CHECK(ida && idb && *ida == repl::Value::i64(1) && *idb == repl::Value::i64(2), "PlayerId 1 and 2");
    CHECK(rig.ep.session_count() == 2, "two sessions live");
}

static void test_kicked_session_answers_nothing() {
    std::printf("a duplicate login kicks a session whose join is still unacknowledged, and the kicked one answers nothing more\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    log_in(rig, a, kA, cyb_record(), t0);
    const auto pa = a.client->packet({a.client->netspeed(200000), a.client->nmt(conn::Nmt::Join)});
    rig.ep.on_datagram(kA, pa.data(), pa.size(), t0 + milliseconds(40));
    rig.ep.tick(t0 + milliseconds(40));
    auto held = rig.take(kA);   // client A receives nothing yet
    // B logs in as the same character from another address
    Joined b = handshake(rig, kB, t0 + milliseconds(50));
    log_in(rig, b, kB, cyb_record(), t0 + milliseconds(60));
    CHECK(rig.ep.session_of(kCyb) && *rig.ep.session_of(kCyb) == kB, "the character is B's now");
    // A's client now receives everything, including the Failure
    bool failure = false, closed = false;
    const auto receive_all = [&](const std::vector<std::vector<uint8_t>>& pkts) {
        for (const auto& p : pkts) {
            auto r = a.client->receive(p);
            for (const auto& m : r.messages) failure = failure || m.type == conn::Nmt::Failure;
            closed = closed || r.closed;
        }
    };
    receive_all(held);
    receive_all(rig.take(kA));
    CHECK(failure && closed, "A got the Failure and the close after its join");
    // A, kicked, still sends requests on its channels
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const auto pkt = a.client->packet({a.client->actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    rig.ep.on_datagram(kA, pkt.data(), pkt.size(), t0 + milliseconds(100));
    rig.ep.tick(t0 + milliseconds(100));
    size_t chains = 0;
    for (const auto& p : rig.take(kA)) chains += a.client->receive(p).delivered.size();
    CHECK(chains == 0, "a kicked (closing) session answers no further requests");
    // B joins fine
    join_now(rig, b, kB, t0 + milliseconds(110));
    CHECK(b.got.size() == 16, "B receives the full join");
    // time passes: A is dropped
    for (int i = 1; i <= 40; ++i) rig.ep.tick(t0 + milliseconds(110 + i * 100));
    CHECK(rig.ep.session_count() == 1, "A is dropped, B stays");
}
// [C | O | A - Cyb]

/// cyb's appearance with `n` decals added.
static std::string cyb_with_decals(size_t n) {
    std::string json = cyb::kAppearanceJsonb;
    const size_t close = json.rfind('}');
    std::string decals = ",\"decalData\":[";
    for (size_t i = 0; i < n; ++i) {
        if (i) decals += ",";
        decals += "{\"guid\":\"" + std::to_string(1000 + i) + "\",\"strength\":1.0,\"scaleU\":1.0,\"scaleV\":1.0,\"slotIndex\":" + std::to_string(i % 200) +
                  ",\"mergeSectionMaterialBase\":\"5\",\"offsetU\":0.5,\"offsetV\":0.5,\"angle\":0.25,\"color\":\"7\",\"discoloration\":0.1}";
    }
    decals += "]";
    json.insert(close, decals);
    return json;
}

/// An appearance of `n` face morphs and nothing else (about 12 bytes of JSON each, about 100 bits each on the wire).
static std::string morphs_only(size_t n) {
    std::string json = "{\"faceMorphWeightMaps\":{";
    for (size_t i = 0; i < n; ++i) {
        if (i) json += ",";
        json += "\"m" + std::to_string(i) + "\":0.5";
    }
    return json + "}}";
}

static void test_unsendable_join() {
    std::printf("a join that can never be sent\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    struct Case { const char* what; std::string json; };
    const Case cases[] = {{"100 decals", cyb_with_decals(100)}, {"5000 morphs", morphs_only(5000)}, {"1500 decals", cyb_with_decals(1500)}};
    for (const Case& c : cases) {
        Rig rig(cfg);
        g_log.str("");
        Joined a = handshake(rig, kA, t0);
        svc::Character rec = cyb_record();
        rec.appearance_json = c.json;
        log_in(rig, a, kA, rec, t0);
        auto got = exchange(rig, a, kA, {a.client->netspeed(200000), a.client->nmt(conn::Nmt::Join)}, t0 + milliseconds(40));
        auto more = exchange(rig, a, kA, {}, t0 + milliseconds(100));
        const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
        if (!failure) failure = find_message(more, conn::Nmt::Failure);
        spdlog::default_logger()->flush();
        const std::string log = g_log.str();
        std::printf("    %s: appearance JSON of %zu bytes, %zu chains arrived, Failure %d\n", c.what, rec.appearance_json.size(), a.got.size(), int(failure != nullptr));
        if (c.what == std::string("100 decals")) {
            CHECK(!failure && a.got.size() == 16, "(control: an appearance with 100 decals, a pawn open of many parts, joins)");
        } else if (c.what == std::string("5000 morphs")) {
            CHECK(rec.appearance_json.size() < bs::kMaxAppearanceBytes, "(5,000 morphs are under the appearance cap)");
            CHECK(failure && failure->error == login_text::kLoginFailed,
                  "a join that cannot be sent (the pawn open's content is over what a chain may carry) ends with the Failure");
            CHECK(log.find("the join cannot be sent: pawn open") != std::string::npos, "and the log says which chain was refused");
            for (int i = 1; i <= 40; ++i) rig.ep.tick(t0 + milliseconds(100 + i * 100));
            CHECK(rig.ep.session_count() == 0, "the session is dropped after the linger");
        } else {
            CHECK(failure && failure->error == login_text::kLoginFailed && a.got.empty() && log.find("appearance: too large") != std::string::npos,
                  "1,500 decals (271 KB of JSON) are refused at the setup, before anything is built: the Failure, nothing sent, 'too large' in the log");
        }
    }
}

static void test_acks_before_requests() {
    std::printf("acknowledgements and a request in the same datagram\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    log_in(rig, a, kA, cyb_record(), t0);
    // Join; the client receives the 15 chains (this acknowledges nothing yet: its next packet will)
    join_now(rig, a, kA, t0 + milliseconds(40));
    CHECK(a.got.size() == 16, "(the join arrived)");
    // one packet: the acks of all of it + the request for the load
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const size_t before = a.got.size();
    const auto req = a.client->packet({a.client->actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    rig.ep.on_datagram(kA, req.data(), req.size(), t0 + milliseconds(100));
    rig.ep.tick(t0 + milliseconds(100));
    drain(rig, a, kA);
    size_t exports = 0, chains = 0;
    for (size_t i = before; i < a.got.size(); ++i) { ++chains; exports += a.got[i].exports.size(); }
    CHECK(chains == 6 && exports == 0, "the acks in a datagram are applied before its requests: the reply leaves out the exports the datagram acknowledged");
}

// ============================================================================================================================
//  the NMT_Login unique-id parser with hostile and odd forms
// ============================================================================================================================
// built by CoA with C.Y.B

static void write_fstring_utf16(BunchWriter& w, const std::u16string& s) {
    w.write_int32(-static_cast<int32_t>(s.size() + 1));
    for (char16_t c : s) w.write_bits(c, 16);
    w.write_bits(0, 16);
}

static std::optional<conn::ControlMessage> parse_bits(const BunchWriter& w) {
    return conn::parse_control_message(w.data(), w.byte_size(), 0, w.bit_pos());
}

static void test_login_tail() {
    std::printf("NMT_Login's unique id in hostile and odd forms\n");
    const auto base = [](BunchWriter& w) {
        w.write_uint8(static_cast<uint8_t>(conn::Nmt::Login));
        w.write_fstring_ansi("0");
        w.write_fstring_ansi("?Name=cyb");
    };
    const auto is_login_without_id = [](const std::optional<conn::ControlMessage>& m) {
        return m && m->type == conn::Nmt::Login && !m->has_unique_id && m->request_url == "?Name=cyb";
    };
    {   // the capture's form
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_fstring_ansi("cyb-id"); w.write_fstring_ansi("NULL");
        const auto m = parse_bits(w);
        CHECK(m && m->has_unique_id && m->unique_id == "cyb-id" && m->platform_name == "NULL" && m->request_url == "?Name=cyb", "the capture's form: flags 8, the id, the platform");
    }
    {   // truncated id: count says 500, 3 bytes follow
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(500); w.write_uint8('a'); w.write_uint8('b'); w.write_uint8('c');
        CHECK(is_login_without_id(parse_bits(w)), "a truncated id (count 500, 3 bytes): still a Login, without the unique id (the reader is restored)");
    }
    {   // id but no platform
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_fstring_ansi("cyb-id");
        CHECK(is_login_without_id(parse_bits(w)), "an id without a platform: a Login, no unique id");
    }
    {   // flags 9 (encoded), junk after
        BunchWriter w; base(w);
        w.write_uint8(9); w.write_uint32(0xdeadbeef); w.write_uint32(0xdeadbeef);
        CHECK(is_login_without_id(parse_bits(w)), "flags 9 (encoded): a Login, no unique id");
    }
    {   // flags 0x10: another subsystem's id
        BunchWriter w; base(w);
        w.write_uint8(0x10); w.write_fstring_ansi("x"); w.write_fstring_ansi("NULL");
        CHECK(is_login_without_id(parse_bits(w)), "flags 0x10 (another subsystem): a Login, no unique id (only the NULL form is read)");
    }
    {   // UTF-16 id with a non-ASCII character
        BunchWriter w; base(w);
        w.write_uint8(8); write_fstring_utf16(w, u"Müller-PC"); w.write_fstring_ansi("NULL");
        const auto m = parse_bits(w);
        CHECK(m && m->has_unique_id && m->unique_id.size() == 9, "a UTF-16 id parses (its non-ASCII character as read_fstring maps it)");
    }
    {   // count 1025
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(1025);
        for (int i = 0; i < 1025; ++i) w.write_uint8('a');
        w.write_fstring_ansi("NULL");
        CHECK(is_login_without_id(parse_bits(w)), "an id over 1024 characters: a Login, no unique id");
    }
    {   // INT32_MIN as the length
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(std::numeric_limits<int32_t>::min()); w.write_fstring_ansi("NULL");
        CHECK(is_login_without_id(parse_bits(w)), "an INT32_MIN length: a Login, no unique id");
    }
    {   // empty id, empty platform
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(0); w.write_int32(0);
        const auto m = parse_bits(w);
        CHECK(m && m->has_unique_id && m->unique_id.empty(), "an empty id and platform: has_unique_id with an empty id");
        std::vector<std::string> notes;
        const auto id = make_login_identity(LoginMessage{m ? m->request_url : "", m && m->has_unique_id, m ? m->unique_id : ""}, cyb_record(), &notes);
        CHECK(id.unique_id == "cyb", "...and the identity falls back to the name option");
    }
    {   // trailing garbage after the platform
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_fstring_ansi("id"); w.write_fstring_ansi("NULL");
        for (int i = 0; i < 40; ++i) w.write_uint8(0xAB);
        const auto m = parse_bits(w);
        CHECK(m && m->has_unique_id && m->unique_id == "id", "trailing garbage after the platform is ignored");
    }
    {   // an embedded NUL then padding (count 10)
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(10);
        const uint8_t raw[10] = {'a', 'b', 0, 'z', 'z', 'z', 'z', 'z', 'z', 'z'};
        for (uint8_t c : raw) w.write_uint8(c);
        w.write_fstring_ansi("NULL");
        const auto m = parse_bits(w);
        CHECK(m && m->type == conn::Nmt::Login, "an id with an embedded NUL still parses as a Login");
    }
    {   // 7 stray bits after the URL
        BunchWriter w; base(w);
        w.write_bits(1, 7);
        CHECK(is_login_without_id(parse_bits(w)), "7 stray bits after the URL: a Login, no unique id");
    }
    {   // the URL itself truncated: not a Login
        BunchWriter w;
        w.write_uint8(static_cast<uint8_t>(conn::Nmt::Login));
        w.write_fstring_ansi("0");
        w.write_int32(300);
        CHECK(!parse_bits(w), "a truncated URL: unreadable");
    }
}
// C-O-A.C.Y.B

/// With AOC_CONTENT_DB naming the real content.db (cd services && go run ./cmd/contentc): the same join against it, in each mode.
static void test_real_content_db() {
    std::printf("against the real content.db (AOC_CONTENT_DB)\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (!env || !*env) {
        std::printf("  [skip] AOC_CONTENT_DB is not set (cd services && go run ./cmd/contentc; AOC_CONTENT_DB=dist\\content\\content.db)\n");
        return;
    }
    for (const auto mode : {bs::GameStateMode::Tier1, bs::GameStateMode::Tier2, bs::GameStateMode::Captured}) {
        Harness h(mode, env);
        const bool joined = h.join();
        h.settle();
        auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), mode);
        auto seq = pb->join_sequence();
        const std::string diff = joined && seq ? compare_sends(h.got, 0, *seq, kChannels) : "no join";
        const char* name = mode == bs::GameStateMode::Tier1 ? "tier1" : mode == bs::GameStateMode::Tier2 ? "tier2" : "captured";
        std::string msg = std::string("the join against the real content.db, GameState ") + name + ": 16 chains equal to the bootstrap's, in order";
        CHECK(joined && !h.s->failed() && h.got.size() == 16 && diff.empty(), msg.c_str());
        if (!diff.empty()) std::printf("    %s\n", diff.c_str());
    }
}

int main() {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    test_identity();
    test_content_missing();
    test_content_built_before_b2();
    test_stat_program_problems();
    test_join_through_endpoint();
    test_setup_refused_at_join();
    test_channels();
    test_join_and_acks();
    test_phase_in_and_moves();
    test_possession_bunch_and_requests();
    test_join_waits_for_the_window();
    test_window_full();
    test_phase_in_waits();
    test_echo_waits();
    test_lost_export();
    test_lost_then_resent();
    test_request_budget();
    test_client_driven_log_lines();
    test_two_players();
    test_kicked_session_answers_nothing();
    test_unsendable_join();
    test_acks_before_requests();
    test_login_tail();
    test_real_content_db();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

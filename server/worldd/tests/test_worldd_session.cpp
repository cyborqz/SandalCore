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
#include "test_registry.h"

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

static const std::array<uint32_t, 4> kChannels = {3, 4, 5, 6};   // PlayerController, GameState, pawn, PlayerState

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

#include "session_registered.inc"


int main() {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    g_fail += aoc_tests::run();   // every check registered in session_registered.inc, in the order of the lines

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

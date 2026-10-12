// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_movement.cpp: movement checks and saving positions, worldd plan A6
//
//  PlayerBootstrap::correction     ClientAdjustPosition decodes back, with the client-faithful reader,
//                                  to the values sent; unreliable, on the pawn's channel, no exports
//  CharacterSaver                  the worker thread: a position is saved, the latest wins, a slow
//                                  service never blocks the caller, the queue is bounded, Unavailable
//                                  is retried and a refusal is not, flush and the destructor are bounded
//  the session (a fake client)     a walk is acknowledged; a teleport or a non-finite number gets
//                                  ClientAdjustPosition with the refused move's TimeStamp and the last good
//                                  position (and no acknowledgement), in the last good move's mode (walking for
//                                  mode 0), once per datagram however many moves were refused in it; a TimeStamp
//                                  that does not rise (back, twice, 0) gets nothing and is not counted as
//                                  refused; the next move near the last good position is acknowledged; pending
//                                  and old moves are judged oldest first; a damaged parameter is neither; the
//                                  captured moves are all acknowledged (the server's clock follows their
//                                  TimeStamps); 36 moves that each claim 5 s, in one datagram, move the player
//                                  no further than the bank buys; --max-move-speed lowers the limit; refusals
//                                  are logged three times at info
//  saving                          the last good position (never a refused one) and yaw are handed to the
//                                  sink: when the session ends, every 60 s while the position changed (not
//                                  before, not when nothing moved); the end hands the position over again
//                                  when it differs from the record's (a periodic save may have been lost) or
//                                  after a hand-over (the player may be back where it started)
//  through the endpoint            with a real CharacterSaver on a fake character service: the client quits,
//                                  times out, is kicked for another login, worldd stops: the character service
//                                  gets the last good position; the 60 s tick; the 60 s save lost three times
//                                  and the player quits standing still: the service still gets the position;
//                                  a duplicate login: the new session starts at the old one's last good
//                                  position and that is saved before anything the new one saves
//  the saver under load            a producer thread against a service that fails 30% of its calls: never an
//                                  older position after a newer one, every character ends with its last
// ============================================================================
#include "bootstrap/captured_setup.h"
#include "capture_blocks.h"
#include "capture_bootstrap.h"
#include "capture_moves.h"
#include "capture_packets.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"
#include "session/player_session.h"
#include "session/position_saver.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
// C | Y | B x C O A

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace fs = std::filesystem;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;

static std::ostringstream g_log;
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
static const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
static const Clock::time_point t0{seconds(5000)};
static const Address kA{0x7F000001u, 50510};
static const Address kB{0x7F000001u, 50511};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;

// the spawn point of cyb (no saved position) and of the captures
static constexpr double kX = -777664.5, kY = 616578.6875, kZ = 15952.0498046875;
static constexpr uint16_t kSpawnYaw = 16550;

static uint8_t g_counter = 1;
static void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_counter++;
}

/// Polls `pred` for up to `limit`.
template <class Pred>
static bool wait_until(Pred pred, milliseconds limit = milliseconds(3000)) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

// ============================================================================================================================
//  a character service that records what it is asked, and can be slow, fail and throw
// ============================================================================================================================

struct FakeCharacters : svc::CharacterApi {
    struct Save {
        std::string    id;
        svc::Position  position;
    };
    std::mutex                 mu;
    std::condition_variable    cv;
    std::vector<Save>          saves;          // the calls that were answered Ok
    std::vector<svc::CallStatus> answers;      // every call's answer
    std::deque<svc::CallStatus> script;        // answers to give next (Ok when empty)
    bool                       block = false;  // calls wait until released (or max_block)
    milliseconds               max_block{2000};
    int                        running = 0;    // calls inside save_position now
    bool                       throw_once = false;

    svc::CharacterResult get_character(const std::string&) override { return {}; }

    svc::CallStatus save_position(const std::string& id, const svc::Position& position) override {
        std::unique_lock<std::mutex> lock(mu);
        ++running;
        cv.notify_all();
        cv.wait_for(lock, max_block, [this] { return !block; });
        svc::CallStatus status = svc::CallStatus::Ok;
        if (!script.empty()) {
            status = script.front();
            script.pop_front();
        }
        --running;
        if (throw_once) {
            throw_once = false;
            answers.push_back(svc::CallStatus::Failed);
            cv.notify_all();
            throw std::runtime_error("the fake service threw");
        }
        answers.push_back(status);
        if (status == svc::CallStatus::Ok) saves.push_back({id, position});
        cv.notify_all();
        return status;
    }
// [C | O | A - C Y B]

    void hold() { std::lock_guard<std::mutex> l(mu); block = true; }
    void release() {
        { std::lock_guard<std::mutex> l(mu); block = false; }
        cv.notify_all();
    }
    size_t calls() { std::lock_guard<std::mutex> l(mu); return answers.size(); }
    size_t saved_count() { std::lock_guard<std::mutex> l(mu); return saves.size(); }
    std::vector<Save> saved() { std::lock_guard<std::mutex> l(mu); return saves; }
    bool in_flight() { std::lock_guard<std::mutex> l(mu); return running > 0; }
};

static svc::Position pos(double x, double y = 0, double z = 0, float yaw = 0) { return {x, y, z, yaw}; }

// ============================================================================================================================
//  PlayerBootstrap::correction
// ============================================================================================================================

static const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

/// The RPC a delivered or built content block calls, read the way the client reads it.
struct DecodedRpc {
    bool ok = false;
    std::string name;
    repl::RpcCall call;
};
static DecodedRpc decode_rpc(const BunchWriter& content, const schema::ClassSchema& cls) {
    DecodedRpc d;
    const auto blocks = repl::read_content_blocks(content.data(), content.bit_pos(), 0);
    if (!blocks || blocks->size() != 1) return d;
    size_t p = (*blocks)[0].payload_pos;
    auto calls = repl::read_rpc_fields(cls, content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, p);
    if (!calls || calls->size() != 1) return d;
    d.call = std::move((*calls)[0]);
    d.name = d.call.rpc->name;
    d.ok = d.call.decoded;
    return d;
}

static const repl::Value* param(const DecodedRpc& d, std::string_view path) {
    const schema::RepCommand* c = repl::rpc_command(*d.call.rpc, path);
    if (!c) return nullptr;
    const auto it = d.call.params.values.find(c->handle);
    return it == d.call.params.values.end() ? nullptr : &it->second;
}

static void test_correction_wire() {
    std::printf("PlayerBootstrap::correction\n");
    auto setup = bs::captured_character_setup();
    if (!setup) {
        std::printf("  [FAIL] the captured character's setup does not build\n");
        std::exit(2);
    }
    bs::PlayerBootstrap pb(std::move(*setup), bs::GameStateMode::Tier2);
    CHECK(pb.ok(), "(the captured character's bootstrap builds)");
    const repl::Vec3d loc{-777664.5, 616578.6875, 15952.0498046875};
    const auto fix = pb.correction(12.375f, loc, repl::Vec3d{}, repl::kMoveWalking);
    CHECK(fix.has_value(), "a correction builds");
    if (!fix) return;
    CHECK(fix->role == bs::Role::Pawn && !fix->opens_channel && !fix->reliable() && fix->emission.exports.empty(),
          "it goes on the pawn's channel, unreliable (the function has no FUNC_NetReliable: flags 0x01820c42), with no exports");
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const DecodedRpc d = decode_rpc(fix->emission.payload, pawn);
    CHECK(d.ok && d.name == "ClientAdjustPosition", "the client's reader finds one RPC of the pawn: ClientAdjustPosition, its parameters read to the last bit");
    if (!d.ok) return;
    const repl::Value* ts = param(d, "TimeStamp");
    const repl::Value* nl = param(d, "NewLoc");
    const repl::Value* nv = param(d, "NewVel");
    const repl::Value* mode = param(d, "ServerMovementMode");
    CHECK(ts && ts->get_if<float>() && *ts->get_if<float>() == 12.375f, "TimeStamp 12.375, the refused move's");
    CHECK(nl && nl->get_if<repl::Vec3d>() && *nl->get_if<repl::Vec3d>() == loc, "NewLoc is the last good position, three doubles");
    CHECK(!nv || (nv->get_if<repl::Vec3d>() && *nv->get_if<repl::Vec3d>() == repl::Vec3d{}), "NewVel is zero (worldd does not simulate)");
    CHECK(mode && mode->get_if<uint64_t>() && *mode->get_if<uint64_t>() == repl::kMoveWalking, "ServerMovementMode is the last good move's: walking (1)");
    const repl::Value* base = param(d, "NewBase");
    const repl::Value* has_base = param(d, "bHasBase");
    const repl::Value* relative = param(d, "bBaseRelativePosition");
    CHECK(!base && (!has_base || *has_base == repl::Value::boolean(false)) && (!relative || *relative == repl::Value::boolean(false)),
          "no base: NewBase unset, bHasBase and bBaseRelativePosition false");
// C:Y:B x C O A

    const auto falling = pb.correction(12.375f, loc, repl::Vec3d{0, 0, -100}, repl::kMoveFalling);
    const DecodedRpc f = falling ? decode_rpc(falling->emission.payload, pawn) : DecodedRpc{};
    const repl::Value* fv = f.ok ? param(f, "NewVel") : nullptr;
    const repl::Value* fm = f.ok ? param(f, "ServerMovementMode") : nullptr;
    CHECK(fv && fv->get_if<repl::Vec3d>() && fv->get_if<repl::Vec3d>()->z == -100 && fm && *fm->get_if<uint64_t>() == repl::kMoveFalling,
          "another velocity and mode come back as given");
    const auto second = pb.correction(12.375f, loc, repl::Vec3d{}, repl::kMoveWalking);
    CHECK(second && second->emission.payload.bit_pos() == fix->emission.payload.bit_pos() && fix->emission.payload.bit_pos() > 2 * 192,
          "a second correction is the same bits (it touches no shadow) and more than its two vectors' 384");
}

// ============================================================================================================================
//  CharacterSaver
// ============================================================================================================================

static CharacterSaver::Config quick(milliseconds destructor_wait = milliseconds(2000)) {
    CharacterSaver::Config c;
    c.retry_delay = milliseconds(5);
    c.destructor_wait = destructor_wait;
    return c;
}

static void test_saver() {
    std::printf("CharacterSaver\n");
    {
        FakeCharacters api;
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(1, 2, 3, 90.5f));
        CHECK(saver.flush(seconds(3)), "a position is saved; flush says everything is");
        const auto s = api.saved();
        CHECK(s.size() == 1 && s[0].id == "aa" && s[0].position.x == 1 && s[0].position.y == 2 && s[0].position.z == 3 && s[0].position.yaw == 90.5f &&
                  saver.saved() == 1 && saver.failed() == 0 && saver.queued() == 0,
              "the character service got the id and the position as given");
    }
    {
        // latest wins: the service is slow, the character moves on
        FakeCharacters api;
        api.hold();
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(1));
        CHECK(wait_until([&] { return api.in_flight(); }), "(the worker is inside the first call)");
        saver.save("aa", pos(2));
        saver.save("aa", pos(3));
        saver.save("aa", pos(4));
        saver.save("bb", pos(10));
        CHECK(saver.queued() == 2 && saver.replaced() == 2, "while it is busy a character has one entry: later positions replace the waiting one (two replaced)");
        api.release();
        CHECK(saver.flush(seconds(3)), "(released)");
        const auto s = api.saved();
        CHECK(s.size() == 3 && s[0].id == "aa" && s[0].position.x == 1 && s[1].id == "aa" && s[1].position.x == 4 && s[2].id == "bb" && s[2].position.x == 10,
              "the first position was already on its way, then the latest of aa (4, not 2 or 3) and bb: three calls, in the order the characters first waited");
    }
    {
        // the caller never waits for the service
        FakeCharacters api;
        api.hold();
        CharacterSaver saver(api, quick());
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 20000; ++i) saver.save("c" + std::to_string(i % 50), pos(i));
        const auto took = std::chrono::steady_clock::now() - begin;
        CHECK(took < seconds(1) && saver.queued() <= 50, "20,000 saves for 50 characters while the service hangs return at once: at most one entry each");
        api.release();
        saver.flush(seconds(3));
    }
    {
        // the bound: more characters than the queue holds
        FakeCharacters api;
        api.hold();
        CharacterSaver::Config cfg = quick();
        cfg.max_queued = 3;
        CharacterSaver saver(api, cfg);
        saver.save("c0", pos(0));
        wait_until([&] { return api.in_flight(); });
        for (int i = 1; i <= 4; ++i) saver.save("c" + std::to_string(i), pos(i));
        CHECK(saver.queued() == 3 && saver.dropped() == 1, "a fourth waiting character: the oldest entry (c1) is dropped and counted");
        api.release();
        saver.flush(seconds(3));
        const auto s = api.saved();
        CHECK(s.size() == 4 && s[0].id == "c0" && s[1].id == "c2" && s[2].id == "c3" && s[3].id == "c4", "c0 (in flight), c2, c3, c4 were saved; c1 was not");
    }
    {
        FakeCharacters api;
        api.script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(7));
        CHECK(saver.flush(seconds(3)) && api.calls() == 3 && saver.saved() == 1 && saver.failed() == 0 && api.saved().size() == 1,
              "an Unavailable service is tried again: two refusals then Ok is one saved position, not a failure");
    }
    {
        FakeCharacters api;
        api.script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(7));
        saver.flush(seconds(3));
        CHECK(api.calls() == 3 && saver.saved() == 0 && saver.failed() == 1 && saver.queued() == 0, "three Unavailable answers (max_attempts) and the position is given up: counted as failed");
    }
    {
        FakeCharacters api;
        api.script = {svc::CallStatus::NotFound};
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(7));
        saver.flush(seconds(3));
        api.script = {svc::CallStatus::Refused};
        saver.save("bb", pos(8));
        saver.flush(seconds(3));
        CHECK(api.calls() == 2 && saver.failed() == 2 && saver.saved() == 0, "NotFound and Refused are final: one call each, no retry");
    }
    {
        // a newer position that arrives while an Unavailable one waits to be tried again is the one that is sent
        FakeCharacters api;
        api.script = {svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver::Config cfg = quick();
        cfg.retry_delay = milliseconds(300);
        CharacterSaver saver(api, cfg);
        saver.save("aa", pos(1));
        wait_until([&] { return api.calls() == 1; });
        saver.save("aa", pos(2));
        CHECK(!saver.flush(milliseconds(30)), "(flush does not report an empty queue while a retry is due)");
        CHECK(saver.flush(seconds(3)), "(after the pause everything is saved)");
        const auto s = api.saved();
        CHECK(s.size() == 1 && s[0].position.x == 2 && api.calls() == 2, "the retry sends the newer position (2), not the old one: two calls, one saved");
    }
    {
        FakeCharacters api;
        api.throw_once = true;
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(1));
        saver.flush(seconds(3));
        saver.save("bb", pos(2));
        CHECK(saver.flush(seconds(3)) && saver.failed() == 1 && saver.saved() == 1, "a call that throws is a failure and the worker goes on: the next position is saved");
    }
    {
        FakeCharacters api;
        api.hold();
        api.max_block = milliseconds(400);
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(1));
        const auto begin = std::chrono::steady_clock::now();
        const bool done = saver.flush(milliseconds(50));
        const auto took = std::chrono::steady_clock::now() - begin;
        CHECK(!done && took >= milliseconds(40) && took < milliseconds(300), "flush(50 ms) on a hung service returns false after about 50 ms");
        api.release();
        CHECK(saver.flush(seconds(3)), "(released: flushed)");
    }
    {
        FakeCharacters api;
        {
            CharacterSaver saver(api, quick());
            saver.save("aa", pos(1));
            saver.save("bb", pos(2));
        }
        CHECK(api.saved_count() == 2, "the destructor flushes what is queued");
    }
    {
        // the destructor does not wait for ever: one call is in flight (held 300 ms), a second waits, the second is left behind
        FakeCharacters api;
        api.hold();
        api.max_block = milliseconds(300);
        const auto begin = std::chrono::steady_clock::now();
        {
            CharacterSaver saver(api, quick(milliseconds(50)));
            saver.save("aa", pos(1));
            wait_until([&] { return api.in_flight(); });
            saver.save("bb", pos(2));
        }
        const auto took = std::chrono::steady_clock::now() - begin;
        const auto s = api.saved();
        CHECK(took < seconds(2) && s.size() == 1 && s[0].id == "aa", "with a hung service the destructor gives up after its bound: the call in flight finished, the waiting one was left behind");
        spdlog::default_logger()->flush();
        CHECK(g_log.str().find("not saved") != std::string::npos, "and it says so");
    }
}
// [CoA + C | Y | B]

// ============================================================================================================================
//  the session
// ============================================================================================================================

struct RecordingSink : PositionSink {
    struct Saved {
        std::string   id;
        svc::Position position;
    };
    std::vector<Saved> saves;
    void save(const std::string& id, const svc::Position& p) override { saves.push_back({id, p}); }
};

struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;

    Fixture() {
        dir = content_fixture::make_temp_dir("worldd_movement_");
        const fs::path db = dir / "content.db";
        if (!content_fixture::make_db(db)) std::exit(2);
        std::string error;
        content = content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

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

static bs::LoginIdentity identity() { return {kMachine, kMachine}; }
static double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

static BunchWriter hex_bits(const capture_bootstrap::Bits& b) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, b.bits);
    return w;
}

static repl::NetworkMove move_at(float ts, double x, double y, double z, uint16_t yaw = kSpawnYaw, uint8_t mode = repl::kMoveWalking) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, 0, 0, 0);
    m.location = repl::quantize_vector(100, x, y, z);
    m.control_rotation.yaw = yaw;
    m.movement_mode = mode;
    return m;
}
// CoA / C.Y.B

/// The client's bunch payload for a ServerMovePacked of `moves` (a content block of the pawn: the field and the packed parameter).
static BunchWriter client_move(const repl::MovePacked& moves) {
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    BunchWriter out;
    if (!f) return out;
    BunchWriter params;
    if (!repl::write_move_packed(params, moves)) return out;
    BunchWriter field;
    if (repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}
static BunchWriter client_move(const repl::NetworkMove& m) {
    repl::MovePacked p;
    p.new_move = m;
    return client_move(p);
}
/// One bunch that carries a ServerMovePacked call for each of `moves` (one datagram's worth, in order).
static BunchWriter client_moves(const std::vector<repl::NetworkMove>& moves) {
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    BunchWriter out;
    if (!f) return out;
    BunchWriter field;
    for (const repl::NetworkMove& m : moves) {
        repl::MovePacked p;
        p.new_move = m;
        BunchWriter params;
        if (!repl::write_move_packed(params, p) || repl::write_field(cls, f->index, params, field) == 0) return BunchWriter();
    }
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}
/// The client's bunch for a captured move: its parameter bits as they were, in the field and the content block.
static BunchWriter client_move_raw(const capture_moves::Move& mv) {
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    const auto bytes = capture_blocks::bytes_of(mv.hex);
    BunchWriter params;
    params.write_bit_range(bytes.data(), 0, mv.bits);
    BunchWriter field;
    BunchWriter out;
    if (!f || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

struct Harness {
    Fixture fx;
    RecordingSink sink;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::string error;
    uint32_t pawn_ch = 0;
// built with C Y B and CoA

    explicit Harness(const MovementConfig& mc = {}, bool with_sink = true) {
        fx.world->set_movement(mc);
        if (with_sink) fx.world->set_position_sink(&sink);
        s = PlayerSession::join(conn, *fx.world, cyb_record(), identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        settle();
    }
    void flush() {
        for (auto& pkt : conn.flush(t)) {
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) got.push_back(d);
        }
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        const std::vector<InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const InBunch& b : in) s->on_bunch(conn, b, t);
        s->end_of_datagram(conn);   // (the Endpoint's call once the datagram's bunches are done)
    }
    void step(milliseconds d = milliseconds(20)) {
        t += d;
        s->tick(conn, t);
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    /// The client sends a ServerMovePacked bunch (unreliable) and the server's answers are collected: the chains on the pawn's channel.
    std::vector<const fake::Delivered*> send_move(const BunchWriter& bunch) {
        const size_t from = got.size();
        client_sends({client.actor(pawn_ch, bunch, false)});
        flush();
        std::vector<const fake::Delivered*> out;
        for (size_t i = from; i < got.size(); ++i)
            if (got[i].channel == pawn_ch) out.push_back(&got[i]);
        return out;
    }
    std::vector<const fake::Delivered*> send_move(const repl::NetworkMove& m) { return send_move(client_move(m)); }
};

static bool is_ack(const fake::Delivered& d, float ts) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!d.exports.empty() || d.reliable || d.content.bit_pos() != 68 || !blocks || blocks->size() != 1) return false;
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    size_t pos_bits = (*blocks)[0].payload_pos;
    const auto h = repl::read_field_header(pawn, d.content.data(), pos_bits + (*blocks)[0].payload_bits, pos_bits);
    if (!h || std::string(pawn.field(h->field)->name) != "ClientMoveResponsePacked") return false;
    aoc::protocol::wire::PacketReader r(d.content.data(), d.content.byte_size(), pos_bits + h->payload_bits);
    r.set_pos(pos_bits);
    return r.read_bits(10) == 645 && std::bit_cast<float>(static_cast<uint32_t>(r.read_bits(32))) == ts;
}

/// The ClientAdjustPosition in a delivered chain, if that is what it is.
static std::optional<DecodedRpc> as_correction(const fake::Delivered& d) {
    DecodedRpc r = decode_rpc(d.content, class_named("PlayerPawn_C"));
    if (!r.ok || r.name != "ClientAdjustPosition") return std::nullopt;
    return r;
}

static bool correction_is(const fake::Delivered& d, float ts, double x, double y, double z, uint8_t mode = repl::kMoveWalking) {
    const auto r = as_correction(d);
    if (!r || d.reliable || !d.exports.empty()) return false;
    const repl::Value* t = param(*r, "TimeStamp");
    const repl::Value* l = param(*r, "NewLoc");
    const repl::Value* m = param(*r, "ServerMovementMode");
    // (a parameter at its default, 0, is not on the wire: an absent TimeStamp is 0)
    const float got_ts = t && t->get_if<float>() ? *t->get_if<float>() : 0.0f;
    return (t || ts == 0.0f) && got_ts == ts && l && l->get_if<repl::Vec3d>() && *l->get_if<repl::Vec3d>() == repl::Vec3d{x, y, z} &&
           (m ? *m->get_if<uint64_t>() : 0) == mode;
}

/// (the spawn point as the moves carry it: 0.01 cm precision)
static bool near(double a, double b) { return std::fabs(a - b) < 0.01; }

static void test_good_moves() {
    std::printf("a walk\n");
    Harness h;
    float ts = 1.0f;
    double x = kX;
    size_t acked = 0;
    bool all_acks = true;
    for (int i = 0; i < 20; ++i) {
        ts += 0.017f;
        x += 10.0;
        const auto replies = h.send_move(move_at(ts, x, kY, kZ));
        all_acks = all_acks && replies.size() == 1 && is_ack(*replies[0], ts);
        acked += replies.size();
    }
    CHECK(all_acks && acked == 20 && h.s->moves_acknowledged() == 20, "20 moves a frame apart are each answered with a ClientMoveResponsePacked of their own TimeStamp");
    CHECK(h.s->moves_refused() == 0 && h.s->corrections_sent() == 0, "none refused, none corrected");
    const GoodPosition& g = h.s->movement().good();
    CHECK(near(g.x, x) && near(g.y, kY) && g.time_stamp == ts && std::fabs(g.yaw_degrees - kSpawnYaw * 360.0 / 65536.0) < 1e-9,
          "the last good position is the last move's (location, TimeStamp, the control rotation's yaw in degrees)");

    // the retail client's own moves, every one of the e2e capture's and of the 2026-09-29 run's, as bunches
    for (const bool native : {false, true}) {
        Harness c;
        size_t answered = 0, acks_ok = 0;
        const capture_moves::Move* first = native ? std::begin(capture_moves::kNative) : std::begin(capture_moves::kE2e);
        const capture_moves::Move* last = native ? std::end(capture_moves::kNative) : std::end(capture_moves::kE2e);
        double first_ts = -1;
        for (const capture_moves::Move* mv = first; mv != last; ++mv) {
            const auto data = capture_blocks::bytes_of(mv->hex);
            const auto parsed = repl::read_move_packed(data.data(), data.size(), 0, mv->bits);
            // the server's clock runs with the capture's TimeStamps: each move arrives at its own pace (moves are paced against
            // the server's clock, so a replay that sent them all at one instant would be the racing the pacing stops)
            if (parsed && first_ts < 0) first_ts = parsed->new_move.time_stamp;
            if (parsed) {
                const Clock::time_point at = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(parsed->new_move.time_stamp - first_ts));
                if (at > c.t) c.t = at;   // (never backwards: the harness has settled for a moment already)
            }
            const auto replies = c.send_move(client_move_raw(*mv));
            answered += replies.size();
            if (replies.size() == 1 && parsed && is_ack(*replies[0], parsed->new_move.time_stamp)) ++acks_ok;
        }
        const size_t n = static_cast<size_t>(last - first);
        CHECK(answered == n && acks_ok == n && c.s->moves_acknowledged() == n && c.s->moves_refused() == 0 && c.s->corrections_sent() == 0,
              native ? "all 1,843 moves of the 2026-09-29 run (walking, sprint, dodge, jumps), as the client sent them: each acknowledged with its own TimeStamp, none refused"
                     : "all 640 moves of the e2e capture, as the client sent them: each acknowledged with its own TimeStamp, none refused");
        const auto last_data = capture_blocks::bytes_of((last - 1)->hex);
        const auto lp = repl::read_move_packed(last_data.data(), last_data.size(), 0, (last - 1)->bits);
        CHECK(lp && c.s->movement().good().time_stamp == lp->new_move.time_stamp && near(c.s->movement().good().x, lp->new_move.location_cm().x) &&
                  near(c.s->movement().good().y, lp->new_move.location_cm().y),
              "(and the last good position is the capture's last move)");
    }
}

static void test_corrections() {
    std::printf("a move that is not plausible\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    const double x = kX + 20.0;
    h.send_move(move_at(1.017f, x, kY, kZ));
    const size_t acked = h.s->moves_acknowledged();
    const GoodPosition before = h.s->movement().good();

    // an independent builder's correction for the same values
    bs::PlayerBootstrap pb(h.s->bootstrap().setup(), bs::GameStateMode::Tier2);
// ~ C | Y | B & CoA ~

    const auto replies = h.send_move(move_at(1.034f, x + 5000.0, kY, kZ));   // 50 m in 17 ms
    CHECK(replies.size() == 1 && correction_is(*replies[0], 1.034f, before.x, before.y, before.z), "a teleport: one ClientAdjustPosition with the move's TimeStamp and the last good position");
    const auto want = pb.correction(1.034f, repl::Vec3d{before.x, before.y, before.z}, repl::Vec3d{}, repl::kMoveWalking);
    CHECK(replies.size() == 1 && want && replies[0]->content.bit_pos() == want->emission.payload.bit_pos() &&
              capture_blocks::same_bit_range(replies[0]->content.data(), want->emission.payload.data(), want->emission.payload.bit_pos(), nullptr),
          "its bits are PlayerBootstrap::correction's");
    CHECK(h.s->moves_acknowledged() == acked && h.s->moves_refused() == 1 && h.s->corrections_sent() == 1 && h.s->movement().good() == before,
          "the move is not acknowledged and the last good position is as it was");

    // the client snaps back and goes on: its next move is near the last good position
    const auto next = h.send_move(move_at(1.051f, x + 10.0, kY, kZ));
    CHECK(next.size() == 1 && is_ack(*next[0], 1.051f), "the next move, near the last good position, is acknowledged");
    const GoodPosition g = h.s->movement().good();

    // a TimeStamp that does not rise (a datagram that arrived out of order, or twice): dropped without a word, as UE does; it is
    // not one of the refused moves and the player is not put back
    const auto back = h.send_move(move_at(0.5f, x + 10.0, kY, kZ));
    CHECK(back.empty() && h.s->moves_refused() == 1 && h.s->corrections_sent() == 1 && h.s->moves_stale() == 1 && h.s->movement().good() == g,
          "a TimeStamp going back: no acknowledgement, no correction, not counted as refused (counted apart), nothing changes");
    const auto again = h.send_move(move_at(g.time_stamp, x + 10.0, kY, kZ));
    CHECK(again.empty() && h.s->moves_refused() == 1 && h.s->corrections_sent() == 1 && h.s->moves_stale() == 2 && h.s->movement().good() == g,
          "the same TimeStamp again (a duplicate datagram): the same");
    const auto zero = h.send_move(move_at(0.0f, x + 10.0, kY, kZ));
    CHECK(zero.empty() && h.s->moves_stale() == 3, "a TimeStamp of 0 (UE's IsClientTimeStampValid: ignored): the same");
    // a non-finite number: a vector at full precision with a NaN
    repl::NetworkMove nan = move_at(1.068f, x + 10.0, kY, kZ);
    nan.location.mode = repl::QuantVec::Mode::RawDouble;
    nan.location.x = static_cast<int64_t>(std::bit_cast<uint64_t>(std::numeric_limits<double>::quiet_NaN()));
    nan.location.y = static_cast<int64_t>(std::bit_cast<uint64_t>(kY));
    nan.location.z = static_cast<int64_t>(std::bit_cast<uint64_t>(kZ));
    const auto nans = h.send_move(nan);
    CHECK(nans.size() == 1 && correction_is(*nans[0], 1.068f, g.x, g.y, g.z), "a Location with a NaN: corrected");
    const auto nan_ts = h.send_move(move_at(std::numeric_limits<float>::quiet_NaN(), x + 10.0, kY, kZ));
    CHECK(nan_ts.size() == 1 && correction_is(*nan_ts[0], 0.0f, g.x, g.y, g.z), "a TimeStamp that is NaN: corrected, with TimeStamp 0 (a NaN names no saved move)");
    const auto far = h.send_move(move_at(1.085f, 2000000.0, kY, kZ));
    CHECK(far.size() == 1 && correction_is(*far[0], 1.085f, g.x, g.y, g.z), "a location outside the world: corrected");
    const auto up = h.send_move(move_at(1.102f, x + 10.0, kY, kZ + 900.0));
    CHECK(up.size() == 1 && correction_is(*up[0], 1.102f, g.x, g.y, g.z), "9 m up in a frame: corrected");
    CHECK(h.s->moves_refused() == 5 && h.s->corrections_sent() == 5, "five refused (the teleport, a NaN location, a NaN TimeStamp, outside the world, 9 m up), five corrections");
    const auto ok = h.send_move(move_at(1.119f, x + 15.0, kY, kZ));
    CHECK(ok.size() == 1 && is_ack(*ok[0], 1.119f), "and the client is acknowledged again once it is plausible");

    // the mode is the last good move's: after a falling move, the correction says falling
    h.send_move(move_at(1.136f, x + 15.0, kY, kZ - 10.0, kSpawnYaw, repl::kMoveFalling));
    const GoodPosition fall = h.s->movement().good();
    const auto fall_fix = h.send_move(move_at(1.153f, x + 9000.0, kY, kZ - 10.0, kSpawnYaw, repl::kMoveFalling));
    CHECK(fall_fix.size() == 1 && correction_is(*fall_fix[0], 1.153f, fall.x, fall.y, fall.z, repl::kMoveFalling), "the correction carries the last good move's movement mode (falling)");

    // a good move whose mode byte is 0 (MOVE_None: the pawn has not landed yet, as in the captures' first moves): the correction never says 0
    h.send_move(move_at(1.17f, x + 15.0, kY, kZ - 10.0, kSpawnYaw, 0));
    const GoodPosition none = h.s->movement().good();
    CHECK(none.movement_mode == 0 && none.time_stamp == 1.17f, "(a move in mode 0 is accepted and kept as it is)");
    const auto none_fix = h.send_move(move_at(1.187f, x + 9000.0, kY, kZ - 10.0, kSpawnYaw, 0));
    CHECK(none_fix.size() == 1 && correction_is(*none_fix[0], 1.187f, none.x, none.y, none.z, repl::kMoveWalking),
          "the correction after it says walking (1), never mode 0: a client put back in mode 0 would not walk");
}

static void test_one_correction_per_datagram() {
    std::printf("at most one correction per datagram\n");
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        const GoodPosition g = h.s->movement().good();
        // five teleports in one datagram: one correction, the latest's TimeStamp
        std::vector<repl::NetworkMove> five;
        for (int i = 0; i < 5; ++i) five.push_back(move_at(2.0f + 0.017f * i, kX + 90000.0 + i, kY, kZ));
        const auto r = h.send_move(client_moves(five));
        CHECK(r.size() == 1 && correction_is(*r[0], five.back().time_stamp, g.x, g.y, g.z) && h.s->moves_refused() == 5 && h.s->corrections_sent() == 1,
              "five refused moves in one datagram: five refusals counted, one ClientAdjustPosition, with the latest refused move's TimeStamp");
        const auto next = h.send_move(move_at(2.2f, kX + 20.0, kY, kZ));
        CHECK(next.size() == 1 && is_ack(*next[0], 2.2f), "(and the next datagram is judged afresh)");
    }
    {
        // a refusal followed by an accepted move in the same datagram: the client is where the server has it, no correction
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        const auto r = h.send_move(client_moves({move_at(2.0f, kX + 90000.0, kY, kZ), move_at(2.017f, kX + 10.0, kY, kZ)}));
        CHECK(r.size() == 1 && is_ack(*r[0], 2.017f) && h.s->moves_refused() == 1 && h.s->corrections_sent() == 0,
              "a refused move, then an accepted one in the same datagram: only the acknowledgement goes out (the later move supersedes)");
    }
    {
        // an accepted move and then a refusal: both answered (an acknowledgement, then the correction)
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        const auto r = h.send_move(client_moves({move_at(2.0f, kX + 10.0, kY, kZ), move_at(2.017f, kX + 90000.0, kY, kZ)}));
        const GoodPosition g = h.s->movement().good();
        CHECK(r.size() == 2 && is_ack(*r[0], 2.0f) && correction_is(*r[1], 2.017f, g.x, g.y, g.z) && near(g.x, kX + 10.0),
              "an accepted move, then a refused one: an acknowledgement and a correction to the accepted position");
    }
}

static void test_racing_the_time_stamp() {
    std::printf("a client that races its TimeStamp\n");
    // 36 moves in one datagram, each claiming 5 s and moving 100 m: 3.6 km if the claims were taken at their word
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    float ts = 1.0f;
    double x = kX;
    std::vector<repl::NetworkMove> race;
    for (int i = 0; i < 36; ++i) {
        ts += 5.0f;
        x += 10000.0;
        race.push_back(move_at(ts, x, kY, kZ));
    }
    const BunchWriter bunch = client_moves(race);
    CHECK(bunch.bit_pos() > 0, "(the datagram of 36 racing moves builds)");
    const auto r = h.send_move(bunch);
    const GoodPosition& g = h.s->movement().good();
    const MoveLimits lim;
    std::printf("    36 racing moves in one datagram: %zu replies, %zu acknowledged, %zu refused; the player is %.0f cm from where it was\n", r.size(), h.s->moves_acknowledged() - 1,
                h.s->moves_refused(), g.x - kX);
    CHECK(g.x - kX <= lim.max_speed * lim.pace_bank + lim.slack + 1.0 && h.s->moves_refused() >= 34,
          "one datagram of 36 moves that claim 5 s each moves the player at most what the bank buys (about 100 m, not 3.6 km); the rest are refused");
    CHECK(h.s->corrections_sent() == 1, "and the 34 or more refusals are one correction");
    h.s->finish("racing");
    CHECK(h.sink.saves.size() <= 1 && (h.sink.saves.empty() || h.sink.saves[0].position.x - kX <= lim.max_speed * lim.pace_bank + lim.slack + 1.0),
          "what the character service gets, if anything, is that position, not the 3.6 km");
}

static void test_pending_and_old() {
    std::printf("pending and old moves\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    // pending teleports, new is at home: the reply is the new move's, accepted
    repl::MovePacked p;
    p.new_move = move_at(1.034f, kX + 10.0, kY, kZ);
    p.pending = move_at(1.017f, kX + 9000.0, kY, kZ);
    auto r = h.send_move(client_move(p));
    CHECK(r.size() == 1 && is_ack(*r[0], 1.034f) && std::fabs(h.s->movement().good().x - (kX + 10.0)) < 0.01,
          "a refused pending move does not condemn the new move: the new one is acknowledged and is the last good position");

    // both plausible: judged oldest first, the last good position is the new one
    repl::MovePacked q;
    q.old = move_at(1.051f, kX + 12.0, kY, kZ);
    q.pending = move_at(1.068f, kX + 14.0, kY, kZ);
    q.new_move = move_at(1.085f, kX + 16.0, kY, kZ);
    r = h.send_move(client_move(q));
    CHECK(r.size() == 1 && is_ack(*r[0], 1.085f) && std::fabs(h.s->movement().good().x - (kX + 16.0)) < 0.01 && h.s->movement().good().time_stamp == 1.085f,
          "old, pending and new move, all plausible: one acknowledgement, of the new move; the last good position is the new move's");

    // a pending move already seen (every packet repeats the last frame) is not judged again, and does not undo the new one
    repl::MovePacked rep;
    rep.new_move = move_at(1.102f, kX + 18.0, kY, kZ);
    rep.pending = move_at(1.085f, kX + 16.0, kY, kZ);   // the previous packet's new move
    r = h.send_move(client_move(rep));
    CHECK(r.size() == 1 && is_ack(*r[0], 1.102f) && std::fabs(h.s->movement().good().x - (kX + 18.0)) < 0.01, "a pending move that is the previous new move (seen already) is skipped");
// -- CoA x C | Y | B --

    // the new move itself going back (a datagram that arrived late) is dropped without a reply, but its plausible pending move still counts
    repl::MovePacked back;
    back.new_move = move_at(1.0f, kX + 18.0, kY, kZ);
    back.pending = move_at(1.119f, kX + 20.0, kY, kZ);
    r = h.send_move(client_move(back));
    CHECK(r.empty() && h.s->moves_stale() == 1 && h.s->moves_refused() == 0 && std::fabs(h.s->movement().good().x - (kX + 20.0)) < 0.01 &&
              h.s->movement().good().time_stamp == 1.119f,
          "a new move whose TimeStamp went back: no reply (neither acknowledged nor corrected); its (plausible) pending move was judged first and moved the player");
}

static void test_unreadable() {
    std::printf("a parameter that does not read\n");
    Harness h;
    BunchWriter good = client_move(move_at(1.0f, kX, kY, kZ));
    // damage a bit of the parameter's own NumBits: the field and the block are as they were, the parameter no longer ends where it says
    const auto parsed = repl::read_client_bunch(class_named("PlayerPawn_C"), [](const emit::FIntrepidNetworkGUID&) { return nullptr; }, good.data(), good.bit_pos());
    const size_t flip = parsed && parsed->rpcs.size() == 1 ? parsed->rpcs[0].call.param_pos + 3 : 0;
    CHECK(flip > 0, "(the move's parameter is found in its bunch)");
    std::vector<uint8_t> bytes(good.data(), good.data() + good.byte_size());
    bytes[flip >> 3] ^= static_cast<uint8_t>(1u << (flip & 7));
    BunchWriter bad;
    bad.write_bit_range(bytes.data(), 0, good.bit_pos());
    g_log.str("");
    const size_t acked = h.s->moves_acknowledged();
    auto r = h.send_move(bad);
    r = h.send_move(bad);
    CHECK(r.empty() && h.s->moves_acknowledged() == acked && h.s->corrections_sent() == 0 && h.s->moves_refused() == 0,
          "a ServerMovePacked that does not read gets neither an acknowledgement nor a correction (the client resends)");
    spdlog::default_logger()->flush();
    size_t lines = 0;
    for (size_t at = g_log.str().find("does not read ("); at != std::string::npos; at = g_log.str().find("does not read (", at + 1)) ++lines;
    CHECK(lines == 1, "and it is logged once, with the reason");
}

static void test_low_limit() {
    std::printf("--max-move-speed\n");
    MovementConfig mc;
    mc.limits.max_speed = 300.0;
    Harness h(mc);
    h.send_move(move_at(1.0f, kX, kY, kZ));
    float ts = 1.0f;
    double x = kX;
    size_t acks = 0, fixes = 0;
    bool fixes_at_last_good = true;
    for (int i = 0; i < 30; ++i) {
        ts += 0.1f;
        x += 60.0;   // 600 cm/s
        const GoodPosition g = h.s->movement().good();
        for (const auto* d : h.send_move(move_at(ts, x, kY, kZ))) {
            if (is_ack(*d, ts)) ++acks;
            else if (correction_is(*d, ts, g.x, g.y, g.z)) ++fixes;
            else fixes_at_last_good = false;
        }
    }
    CHECK(fixes_at_last_good && fixes >= 25 && acks <= 3, "at a limit of 300 cm/s a walk at 600 cm/s is mostly corrected, each time to the last good position (walking snaps back)");
    CHECK(h.s->movement().limits().max_speed == 300.0, "(the session has the world's limit)");
}

static void test_refusal_logging() {
    std::printf("logging refusals\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    g_log.str("");
    float ts = 1.0f;
    for (int i = 0; i < 30; ++i) {
        ts += 0.017f;
        h.send_move(move_at(ts, kX + 50000.0, kY, kZ));
    }
    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    size_t info = 0, debug = 0;
    size_t from = 0;
    for (size_t at = log.find("is refused"); at != std::string::npos; at = log.find("is refused", at + 1)) {
        const size_t line = log.rfind('\n', at);
        const std::string head = log.substr(line == std::string::npos ? 0 : line, at - (line == std::string::npos ? 0 : line));
        if (head.find("[info]") != std::string::npos) ++info;
        else if (head.find("[debug]") != std::string::npos) ++debug;
        from = at;
    }
    (void)from;
    CHECK(info == PlayerSession::kLoggedPerKind && debug == 30 - PlayerSession::kLoggedPerKind, "30 refusals: the first three at info, the rest at debug");
    CHECK(log.find("too fast along the ground") != std::string::npos && log.find("put back at") != std::string::npos, "each line says why and where the player is put back");
}

static void test_saving_in_the_session() {
    std::printf("saving, in the session\n");
    {
        Harness h;
        h.s->finish("test");
        CHECK(h.sink.saves.empty(), "a session that never moved saves nothing at its end");
    }
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        h.s->finish("test");
        CHECK(h.sink.saves.empty(), "moves that stayed at the loaded position save nothing");
    }
    {
        Harness h;
        float ts = 1.0f;
        double x = kX;
        for (int i = 0; i < 10; ++i) {
            ts += 0.017f;
            x += 20.0;
            h.send_move(move_at(ts, x, kY, kZ + 1.0, 8192, repl::kMoveWalking));
        }
        h.send_move(move_at(ts + 0.017f, x + 99999.0, kY, kZ));   // refused: never saved
        h.s->finish("the client quit");
        CHECK(h.sink.saves.size() == 1 && h.sink.saves[0].id == kCyb, "the end of the session hands one position to the sink, with the character's id");
        if (h.sink.saves.size() == 1) {
            const svc::Position& p = h.sink.saves[0].position;
            CHECK(std::fabs(p.x - x) < 0.01 && std::fabs(p.y - kY) < 0.01 && std::fabs(p.z - (kZ + 1.0)) < 0.01 && std::fabs(p.yaw - 45.0f) < 1e-4,
                  "it is the last good position, not the refused teleport: x, y, z in cm and the yaw of the control rotation in degrees (8192 is 45)");
        }
        h.s->finish("again");
        CHECK(h.sink.saves.size() == 1 && h.s->positions_handed_over() == 1, "finish is once: a second call saves nothing");
    }
    {
        // the 60 s tick: it starts at the first tick, saves a changed position, and not the same one twice
        Harness h;
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);   // starts the interval
        h.send_move(move_at(1.0f, kX + 10.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(59));
        CHECK(h.sink.saves.empty(), "59 s after the first tick: nothing is saved yet");
        h.s->tick(h.conn, start + seconds(60));
        CHECK(h.sink.saves.size() == 1 && std::fabs(h.sink.saves[0].position.x - (kX + 10.0)) < 0.01, "at 60 s the position that changed is saved");
        h.s->tick(h.conn, start + seconds(120));
        CHECK(h.sink.saves.size() == 1, "at 120 s nothing moved since: no second save of the same position");
        h.send_move(move_at(1.017f, kX + 30.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(179));
        CHECK(h.sink.saves.size() == 1, "(a move at 120 s is not saved at 179 s)");
        h.s->tick(h.conn, start + seconds(180));
        CHECK(h.sink.saves.size() == 2 && std::fabs(h.sink.saves[1].position.x - (kX + 30.0)) < 0.01, "but at 180 s");
        h.s->finish("end");
        CHECK(h.sink.saves.size() == 3 && std::fabs(h.sink.saves[2].position.x - (kX + 30.0)) < 0.01,
              "the end of the session hands over the last good position again, although nothing moved since the last save: the saver may have lost that one, and the record is what the session compares with");
        h.send_move(move_at(1.034f, kX + 40.0, kY, kZ));   // after finish: still judged, not saved
        h.s->tick(h.conn, start + seconds(240));
        CHECK(h.sink.saves.size() == 3, "after finish the session saves no more");
    }
    {
        // away and back: a periodic save took a position that is not the record's; the player is back at the record's when the
        // session ends. The end must hand that over too, or the record keeps the position of the periodic save.
        Harness h;
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);
        h.send_move(move_at(1.0f, kX + 500.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(60));
        CHECK(h.sink.saves.size() == 1 && std::fabs(h.sink.saves[0].position.x - (kX + 500.0)) < 0.01, "(the 60 s save holds the position 5 m away)");
        h.send_move(move_at(1.5f, kX, kY, kZ));
        h.s->finish("end");
        CHECK(h.sink.saves.size() == 2 && std::fabs(h.sink.saves[1].position.x - kX) < 0.01,
              "back at the loaded position when the session ends: the end hands it over, so the record is not left with the position of the 60 s save");
    }
    {
        // a refused move never reaches a save, even when the periodic save follows it
        Harness h;
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);
        h.send_move(move_at(1.0f, kX + 5.0, kY, kZ));
        h.send_move(move_at(1.017f, kX + 90000.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(61));
        CHECK(h.sink.saves.size() == 1 && std::fabs(h.sink.saves[0].position.x - (kX + 5.0)) < 0.01, "a periodic save after a refused move holds the last good position");
    }
    {
        // a custom interval
        MovementConfig mc;
        mc.save_interval = seconds(10);
        Harness h(mc);
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);
        h.send_move(move_at(1.0f, kX + 5.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(10));
        CHECK(h.sink.saves.size() == 1, "the interval is the world's save_interval");
    }
    {
        // no sink: moves are checked, nothing is saved, nothing breaks
        Harness h({}, /*with_sink=*/false);
        h.send_move(move_at(1.0f, kX + 5.0, kY, kZ));
        h.s->tick(h.conn, h.t);
        h.s->tick(h.conn, h.t + seconds(100));
        h.s->finish("end");
        CHECK(h.s->positions_handed_over() == 0 && h.s->moves_acknowledged() == 1, "without a PositionSink nothing is handed over");
    }
    {
        // a yaw alone is saved at the end of the session, not by the periodic tick
        Harness h;
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);
        h.send_move(move_at(1.0f, kX, kY, kZ, 4000));
        h.s->tick(h.conn, start + seconds(61));
        CHECK(h.sink.saves.empty(), "turning on the spot is not 'moved': the periodic save skips it");
        h.s->finish("end");
        CHECK(h.sink.saves.size() == 1 && std::fabs(h.sink.saves[0].position.yaw - 4000.0f * 360.0f / 65536.0f) < 1e-3, "but the end of the session saves the new yaw");
    }
}

// ============================================================================================================================
//  through the endpoint, with a real CharacterSaver on a fake character service
// ============================================================================================================================

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
        : ep(std::move(cfg), signer, logins, [this](const Address& a, const std::vector<uint8_t>& p) { sent.emplace_back(a, p); }, counting_random, t0) {}

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
    std::vector<fake::Delivered> got;
};

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

static void drain(Rig& rig, Joined& j, const Address& addr) {
    for (const auto& p : rig.take(addr)) {
        const auto r = j.client->receive(p);
        for (const auto& d : r.delivered) j.got.push_back(d);
    }
}

static void exchange(Rig& rig, Joined& j, const Address& addr, std::vector<fake::Bunch> bunches, Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}

static void log_in(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(j.session, r);
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login(kMachine, kMachine)}, now + milliseconds(2));
}
// ~ Cyb :: C O A ~

/// Hello, login, Join: the player is in the world.
static void join_world(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    log_in(rig, j, addr, rec, now);
    exchange(rig, j, addr, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)}, now + milliseconds(40));
}

static void walk(Rig& rig, Joined& j, const Address& addr, uint32_t pawn_ch, float ts, double x, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->actor(pawn_ch, client_move(move_at(ts, x, kY, kZ, 8192)), false)}, now);
}

struct Endpoints {
    Fixture fx;
    FakeCharacters api;
    std::unique_ptr<CharacterSaver> saver;
    EndpointConfig cfg;
    std::unique_ptr<Rig> rig;
    Endpoints() {
        saver = std::make_unique<CharacterSaver>(api, quick());
        fx.world->set_position_sink(saver.get());
        cfg.players = fx.world.get();
        rig = std::make_unique<Rig>(cfg);
    }
    ~Endpoints() {
        rig.reset();
        saver.reset();
    }
    /// Waits for the saver and returns the saves.
    std::vector<FakeCharacters::Save> saves() {
        saver->flush(seconds(3));
        return api.saved();
    }
};

static void test_endpoint_saves() {
    std::printf("through the endpoint\n");
    {
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        const uint32_t ch = 5;
        walk(rig, j, kA, ch, 1.0f, kX + 10.0, t0 + milliseconds(100));
        walk(rig, j, kA, ch, 1.017f, kX + 25.0, t0 + milliseconds(120));
        CHECK(rig.ep.session_count() == 1 && e.api.saved_count() == 0, "(a player in the world, two moves, nothing saved yet)");
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && s[0].id == kCyb && std::fabs(s[0].position.x - (kX + 25.0)) < 0.01 &&
                  std::fabs(s[0].position.yaw - 45.0f) < 1e-4,
              "the client quits: the character service gets the last good position (x +25 cm, yaw 45) for the character, once");
    }
    {
        // the client vanishes: the session times out (60 s while playing) and the position is saved
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        walk(rig, j, kA, 5, 1.0f, kX + 40.0, t0 + milliseconds(100));
        rig.ep.tick(t0 + seconds(30));
        CHECK(rig.ep.session_count() == 1 && e.saves().empty(), "(30 s of silence: still there, nothing saved)");
        rig.ep.tick(t0 + seconds(70));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && std::fabs(s[0].position.x - (kX + 40.0)) < 0.01, "the client times out: its last good position is saved");
        g_log.str("");
    }
    {
        // a second login of the character kicks the first session: its position is saved when it closes
        Endpoints e;
        Rig& rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        walk(rig, a, kA, 5, 1.0f, kX + 60.0, t0 + milliseconds(100));
        Joined b = handshake(rig, kB, t0 + milliseconds(200));
        LoginResult r;
        r.accepted = true;
        r.character = cyb_record();
        exchange(rig, b, kB, {b.client->hello()}, t0 + milliseconds(210));
        rig.ep.post_login_result(b.session, r);
        rig.ep.tick(t0 + milliseconds(220));
        rig.ep.tick(t0 + seconds(3));   // the kicked session lingers 2 s, then closes
        const auto s = e.saves();
        CHECK(s.size() == 1 && s[0].id == kCyb && std::fabs(s[0].position.x - (kX + 60.0)) < 0.01, "kicked for a login elsewhere: the first session's last good position is saved");
    }
    {
        // worldd stops: finish_players hands over what the open sessions have, and the later removal does not save again
        Endpoints e;
        Rig& rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        walk(rig, a, kA, 5, 1.0f, kX + 70.0, t0 + milliseconds(100));
        rig.ep.kick_all(login_text::kShuttingDown, t0 + milliseconds(200));
        rig.ep.finish_players("worldd is stopping");
        const auto s = e.saves();
        CHECK(s.size() == 1 && std::fabs(s[0].position.x - (kX + 70.0)) < 0.01, "worldd stops: finish_players saves the open session's position");
        rig.ep.tick(t0 + seconds(5));   // the session is removed: nothing more to save
        CHECK(rig.ep.session_count() == 0 && e.saves().size() == 1, "and the session's removal afterwards saves nothing again");
    }
    {
        // the 60 s tick through the endpoint, the client alive (it acknowledges every few seconds)
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        walk(rig, j, kA, 5, 1.0f, kX + 80.0, t0 + milliseconds(100));
        size_t saves_at_30 = 0;
        for (int sec = 1; sec <= 65; ++sec) {
            exchange(rig, j, kA, {}, t0 + seconds(sec));
            if (sec == 30) saves_at_30 = e.api.saved_count() + e.saver->queued();
        }
        e.saver->flush(seconds(3));
        const auto s = e.api.saved();
        CHECK(saves_at_30 == 0 && s.size() == 1 && std::fabs(s[0].position.x - (kX + 80.0)) < 0.01,
              "a player who moved and stays connected is saved once at about 60 s, not before, and not again while still");
    }
    {
        // a character that never moved is not written
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        CHECK(rig.ep.session_count() == 0 && e.saves().empty(), "a client that joins and quits without moving saves nothing (its record is left as it was)");
    }
    {
        // the 60 s save is lost (the service was down for all three tries); the player quits standing still: the end of the session
        // hands the position over again, so the service ends with the last good position
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        walk(rig, j, kA, 5, 1.0f, kX + 80.0, t0 + milliseconds(100));
        e.api.script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable};
        for (int sec = 1; sec <= 65; ++sec) exchange(rig, j, kA, {}, t0 + seconds(sec));
        e.saver->flush(seconds(3));
        CHECK(e.api.calls() == 3 && e.api.saved_count() == 0 && e.saver->failed() == 1,
              "(the 60 s save: three Unavailable answers, given up on; the service holds nothing)");
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + seconds(66));
        rig.ep.tick(t0 + seconds(67));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && std::fabs(s[0].position.x - (kX + 80.0)) < 0.01,
              "the player quits standing still: the end of the session hands the last good position over again, and the service has it");
    }
    {
        // a duplicate login (a crash and a quick relog look the same): B logs in while A plays. The record B's login read is
        // the one in the character service, which has nothing of A's moves yet: B starts where A is, and A's position is handed
        // to the saver before anything B saves
        Endpoints e;
        Rig& rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        walk(rig, a, kA, 5, 1.0f, kX + 60.0, t0 + milliseconds(100));
        walk(rig, a, kA, 5, 1.017f, kX + 70.0, t0 + milliseconds(120));
        Joined b = handshake(rig, kB, t0 + milliseconds(200));
        join_world(rig, b, kB, cyb_record(), t0 + milliseconds(210));   // the record as GetCharacter had it: no saved position
        const PlayerSession* pb = rig.ep.player_at(kB);
        CHECK(pb && near(pb->movement().good().x, kX + 70.0) && near(pb->movement().good().y, kY),
              "the new session starts at A's last good position (x +70 cm), not at the record's spawn point");
        CHECK(e.saves().size() == 1 && near(e.saves()[0].position.x, kX + 70.0), "and A's position was handed to the saver when B took over");
        rig.ep.tick(t0 + seconds(3));   // A's linger ends: it is removed
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 1 && s.size() == 1 && near(s[0].position.x, kX + 70.0),
              "A's removal saves nothing more, and B (which has not moved) saves nothing: the character service ends with A's position");
        // B moves on and quits: its position is the last one, after A's
        walk(rig, b, kB, 5, 1.0f, kX + 90.0, t0 + seconds(4));
        exchange(rig, b, kB, {b.client->close_channel()}, t0 + seconds(5));
        rig.ep.tick(t0 + seconds(6));
        const auto s2 = e.saves();
        CHECK(s2.size() == 2 && near(s2[0].position.x, kX + 70.0) && near(s2[1].position.x, kX + 90.0), "B's own position lands after A's: 70, then 90");
    }
}

// ============================================================================================================================
//  the saver under load: order and latest-wins with a concurrent producer and a service that fails now and then
// ============================================================================================================================

/// A character service that answers Unavailable to a third of its calls, and notes any position that lands after a newer one.
struct OrderingService : svc::CharacterApi {
    std::mutex mu;
    std::map<std::string, double> last_ok;   // per character: the x of the last position the service accepted
    size_t calls = 0, ok = 0, regressions = 0, unavailable = 0;
    std::mt19937_64 rng{99};
    svc::CharacterResult get_character(const std::string&) override { return {}; }
    svc::CallStatus save_position(const std::string& id, const svc::Position& p) override {
        std::lock_guard<std::mutex> l(mu);
        ++calls;
        // a call takes a moment: the producer (the net thread) runs ahead of the worker, as it does against a slow service
        for (const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(50); std::chrono::steady_clock::now() < until;) {
        }
        if (rng() % 10 < 3) {
            ++unavailable;
            return svc::CallStatus::Unavailable;
        }
        const auto it = last_ok.find(id);
        if (it != last_ok.end() && p.x < it->second) ++regressions;   // an older position landed after a newer one
        last_ok[id] = p.x;
        ++ok;
        return svc::CallStatus::Ok;
    }
};

static void test_saver_ordering() {
    std::printf("the saver under load\n");
    OrderingService api;
    CharacterSaver::Config cfg;
    cfg.retry_delay = milliseconds(1);
    cfg.max_attempts = 12;   // (0.3^12 of a position failing for good: none, with this seed)
    cfg.destructor_wait = seconds(10);
    constexpr int kSaves = 1000000, kCharacters = 40;
    std::map<std::string, double> submitted_last;
    size_t replaced = 0, dropped = 0, failed = 0;
    const auto begin = std::chrono::steady_clock::now();
    {
        CharacterSaver saver(api, cfg);
        std::thread producer([&] {   // the net thread's part, on a thread of its own, while the worker saves
            for (int i = 1; i <= kSaves; ++i) {
                const std::string id = "c" + std::to_string(i % kCharacters);
                saver.save(id, pos(i));
                submitted_last[id] = i;
                if ((i & 31) == 0) std::this_thread::yield();   // (the mutex is not fair: a producer that never pauses starves the worker)
            }
        });
        producer.join();
        saver.flush(seconds(20));
        replaced = saver.replaced();
        dropped = saver.dropped();
        failed = saver.failed();
    }
    const auto took = std::chrono::steady_clock::now() - begin;
    size_t last_is_final = 0;
    for (const auto& [id, x] : submitted_last) last_is_final += (api.last_ok.count(id) && api.last_ok[id] == x);
    std::printf("    %d saves for %d characters in %lld ms: %zu service calls, %zu Ok, %zu Unavailable, replaced %zu, dropped %zu, failed %zu\n", kSaves, kCharacters,
                static_cast<long long>(std::chrono::duration_cast<milliseconds>(took).count()), api.calls, api.ok, api.unavailable, replaced, dropped, failed);
    CHECK(api.calls > 100 && api.unavailable > 20 && api.regressions == 0, "30% of the calls are answered Unavailable and retried: the service never sees an older position after a newer one");
    CHECK(dropped == 0 && failed == 0 && last_is_final == static_cast<size_t>(kCharacters),
          "and every character ends with the last position submitted (latest wins; nothing dropped or given up)");
}

int main() {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    test_correction_wire();
    test_saver();
    test_good_moves();
    test_corrections();
    test_one_correction_per_datagram();
    test_racing_the_time_stamp();
    test_pending_and_old();
    test_unreadable();
    test_low_limit();
    test_refusal_logging();
    test_saving_in_the_session();
    test_endpoint_saves();
    test_saver_ordering();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

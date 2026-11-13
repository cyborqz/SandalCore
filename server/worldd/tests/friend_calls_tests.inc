// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_calls_tests.inc
//
//  Registered tests of test_worldd_area_session: social step 7, the friends list after the store.  The PlayerController's SocialComponent
//  (UIntrepidSocialComponent) carries the character's friends and the pending requests in its first block, and every change is one fast-array
//  delta on that component.  The friends' calls (a request, an accept, a decline, a removal and the name check) change the session's two lists
//  and the other side's (a session in this world, or the character service when the other side is offline); the online flag follows the
//  sessions in this world.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================
#include "protocol/repl/fast_array.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/wire/ue5_primitives.h"

#include <spdlog/sinks/ostream_sink.h>

#include <thread>

namespace {

/// The test runner writes its checks to a redirected file: a crash would lose a buffered tail, so this file's runs are unbuffered and the last line printed is
/// the one that was being run.
struct FriendUnbuffered {
    FriendUnbuffered() { std::setvbuf(stdout, nullptr, _IONBF, 0); }
} friend_unbuffered;

using friend_guid = std::array<uint32_t, 4>;

/// The three characters of these tests (lower-case 32 hex digits, none all zero, none the same).
static const std::string kFriendA = "bbec5c86eb101fe5f38c87f0fa22e7d0";   // cyb, the character the fixture's kit belongs to
static const std::string kFriendB = "0123456789abcdef0123456789abcdef";
static const std::string kFriendC = "fedcba9876543210fedcba9876543210";

/// A time far in the future (2099) for the store's expiry in milliseconds.
static constexpr int64_t kFriendLiveExpiryMs = 4102444800000ll;

/// 14 days in milliseconds: the lifetime of a new request (the fixed rule and the game data's PendingFriendRequestTimeInterval).
static constexpr int64_t kFriendRequestLifetimeMs = 14ll * 24 * 60 * 60 * 1000;

/// The wall clock in Unix milliseconds (what the session stamps a new request with).
static int64_t friend_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/// The SocialComponent's schema, found once.
static const schema::ClassSchema* friend_schema() {
    static const schema::ClassSchema* s = gen::find_component_schema("SocialComponent");
    return s;
}

/// A social-component item's fields, as the generated layout puts them on the wire.  The second half holds a pending request's fields.
struct FriendItem {
    friend_guid  guid{};
    std::string  name;
    int64_t      level = 0;
    uint64_t     class_id = 0;
    uint64_t     sub_archetype = 0;
    std::string  guild;
    bool         online = false;
    int64_t      last_online = 0;
    bool         still_friend = false;
    bool         complete = false;
    friend_guid  sender_guid{};
    std::string  sender_name;
    friend_guid  recipient_guid{};
    int64_t      expires = 0;
};

/// Reads a friend item (12 commands) or a pending request (10 commands); the handles are the element commands' 1-based positions.
static bool friend_item_of(const repl::ElementState& es, bool request, FriendItem& out) {
    const auto word = [&](uint16_t handle, uint32_t& into) {
        const auto it = es.values.find(handle);
        const int64_t* v = it == es.values.end() ? nullptr : it->second.get_if<int64_t>();
        if (!v) return false;
        into = static_cast<uint32_t>(*v);
        return true;
    };
    const auto number = [&](uint16_t handle, int64_t& into) {
        const auto it = es.values.find(handle);
        const int64_t* v = it == es.values.end() ? nullptr : it->second.get_if<int64_t>();
        if (!v) return false;
        into = *v;
        return true;
    };
    const auto text = [&](uint16_t handle, std::string& into) {
        const auto it = es.values.find(handle);
        const std::string* v = it == es.values.end() ? nullptr : it->second.get_if<std::string>();
        if (!v) return false;
        into = *v;
        return true;
    };
    const auto flag = [&](uint16_t handle, bool& into) {
        const auto it = es.values.find(handle);
        const bool* v = it == es.values.end() ? nullptr : it->second.get_if<bool>();
        if (!v) return false;
        into = *v;
        return true;
    };
    const auto en = [&](uint16_t handle, uint64_t& into) {
        const auto it = es.values.find(handle);
        const uint64_t* v = it == es.values.end() ? nullptr : it->second.get_if<uint64_t>();
        if (!v) return false;
        into = *v;
        return true;
    };
    if (es.values.size() != (request ? 10u : 12u)) return false;
    bool ok = true;
    if (request) {
        ok = word(1, out.sender_guid[0]) && word(2, out.sender_guid[1]) && word(3, out.sender_guid[2]) && word(4, out.sender_guid[3]);
        ok = ok && text(5, out.sender_name);
        ok = ok && word(6, out.recipient_guid[0]) && word(7, out.recipient_guid[1]) && word(8, out.recipient_guid[2]) && word(9, out.recipient_guid[3]);
        ok = ok && number(10, out.expires);
    } else {
        ok = word(1, out.guid[0]) && word(2, out.guid[1]) && word(3, out.guid[2]) && word(4, out.guid[3]);
        ok = ok && text(5, out.name);
        ok = ok && number(6, out.level);
        ok = ok && en(7, out.class_id) && en(8, out.sub_archetype);
        ok = ok && text(9, out.guild);
        ok = ok && flag(10, out.online);
        ok = ok && number(11, out.last_online);
        ok = ok && flag(12, out.still_friend);
    }
    out.complete = ok;
    return ok;
}

/// The four words of a 32-hex-digit character or friend GUID, or all zero for a value that is not one.
static friend_guid friend_words(const std::string& hex) {
    const auto words = bs::character_guid_words(hex);
    return words ? *words : friend_guid{};
}

/// One block of the SocialComponent in a delivered chain: its fast-array fields and where it starts and ends in the chain.
struct FriendBlock {
    bool                              found = false;
    bool                              has_rep = false;
    size_t                            start_bits = 0;   // the content block's first bit in the chain
    size_t                            payload_bits = 0;
    size_t                            block_bits = 0;   // the content block, header included
    std::vector<repl::FastArrayField> fields;
};

/// The SocialComponent subobject of a session's PlayerController, or nullptr.
static const repl::Subobject* friend_social_of(const PlayerSession& s) {
    for (const repl::Subobject& sub : s.bootstrap().player_controller().subobjects) {
        if (sub.name == "SocialComponent") return &sub;
    }
    return nullptr;
}

/// Decodes the SocialComponent's block of one delivered chain (the last block of the XPTracker chain's join, or the one block of an update).
static std::optional<FriendBlock> friend_block_of(const fake::Delivered& d, const repl::Subobject& social) {
    if (d.close || d.content.bit_pos() == 0) return std::nullopt;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return std::nullopt;
    for (size_t i = 0; i < blocks->size(); ++i) {
        const repl::BlockView& b = (*blocks)[i];
        if (b.header.is_actor || !(b.header.object == social.guid)) continue;
        FriendBlock out;
        out.found = true;
        out.has_rep = b.header.has_rep_layout;
        out.payload_bits = b.payload_bits;
        out.start_bits = i == 0 ? 0 : (*blocks)[i - 1].payload_pos + (*blocks)[i - 1].payload_bits;
        out.block_bits = b.payload_pos + b.payload_bits - out.start_bits;
        size_t pos = b.payload_pos;
        const size_t end = pos + b.payload_bits;
        if (out.has_rep) {
            if (!repl::read_rep_block(*social.obj.schema, d.content.data(), end, pos, social.obj.opts)) return std::nullopt;
        }
        if (pos < end) {
            auto fields = repl::read_fast_array_fields(*social.obj.schema, d.content.data(), end, pos, social.obj.opts);
            if (!fields) return std::nullopt;
            out.fields = std::move(*fields);
        }
        return out;
    }
    return std::nullopt;
}

/// The field of a block for the schema's fast array named `name` (FriendsList / PendingFriendRequests), or nullptr.
static const repl::FastArrayField* friend_field(const FriendBlock& b, const char* name) {
    const schema::ClassSchema* social = friend_schema();
    const schema::NetField* f = social ? social->field(name) : nullptr;
    if (!f) return nullptr;
    for (const repl::FastArrayField& field : b.fields) {
        if (field.field == f->index) return &field;
    }
    return nullptr;
}

/// A number of bits from `start` of a writer, as lower-case hex (the bits packed LSB first, as the block's own bytes).
static std::string friend_bits_hex(const emit::BunchWriter& w, size_t start, size_t bits) {
    std::string out;
    for (size_t i = 0; i < (bits + 7) / 8; ++i) {
        uint32_t byte = 0;
        for (size_t k = 0; k < 8 && start + i * 8 + k < start + bits; ++k) {
            const size_t bit = start + i * 8 + k;
            byte |= static_cast<uint32_t>((w.data()[bit >> 3] >> (bit & 7)) & 1u) << k;
        }
        char text[3];
        std::snprintf(text, sizeof text, "%02x", byte);
        out += text;
    }
    return out;
}

/// Every SocialComponent block of the chains [from, ...) of player `i`.
static std::vector<FriendBlock> friend_blocks_of(const PlayerSession& s, const std::vector<fake::Delivered>& got, size_t from) {
    std::vector<FriendBlock> out;
    const repl::Subobject* social = friend_social_of(s);
    if (!social) return out;
    for (size_t i = from; i < got.size(); ++i) {
        if (auto b = friend_block_of(got[i], *social)) out.push_back(std::move(*b));
    }
    return out;
}

/// One export entry as the client reads it (UPackageMapClient::ReceiveNetGUIDBunch).
struct FriendExport {
    emit::FIntrepidNetworkGUID guid;
    uint8_t                    flags = 0;
    emit::FIntrepidNetworkGUID outer;
    std::string                path;
    uint32_t                   checksum = 0;
};

static bool friend_read_export(const uint8_t* data, size_t len, size_t& pos, FriendExport& out, int depth) {
    if (depth > 8) return false;
    out.guid = emit::read_intrepid_guid(data, len, pos);
    if (out.guid.ObjectId == 0) return true;
    out.flags = static_cast<uint8_t>(ue5::read_bits(data, len, pos, 8));
    if (out.flags & 1) {
        FriendExport outer;
        if (!friend_read_export(data, len, pos, outer, depth + 1)) return false;
        out.outer = outer.guid;
        const int32_t n = static_cast<int32_t>(ue5::read_bits(data, len, pos, 32));
        if (n < 0 || n > 512) return false;
        for (int32_t i = 0; i < n; ++i) {
            const char c = static_cast<char>(ue5::read_bits(data, len, pos, 8));
            if (c != 0) out.path.push_back(c);
        }
        if (out.flags & 4) out.checksum = static_cast<uint32_t>(ue5::read_bits(data, len, pos, 32));
    }
    return true;
}

static std::vector<FriendExport> friend_exports_of(const fake::Delivered& d) {
    std::vector<FriendExport> out;
    for (const emit::BunchWriter& e : d.exports) {
        const size_t bits = e.bit_pos();
        const uint8_t* data = e.data();
        const size_t len = e.byte_size() + 1;
        size_t pos = 0;
        (void)ue5::read_bits(data, len, pos, 1);
        const uint32_t count = static_cast<uint32_t>(ue5::read_bits(data, len, pos, 32));
        for (uint32_t i = 0; i < count && pos < bits; ++i) {
            FriendExport r;
            if (!friend_read_export(data, len, pos, r, 0)) break;
            out.push_back(std::move(r));
        }
    }
    return out;
}

/// The one export of the SocialComponent in `d`, copied.
static std::optional<FriendExport> friend_social_export(const fake::Delivered& d, const repl::Subobject& social) {
    for (const FriendExport& e : friend_exports_of(d)) {
        if (e.guid == social.guid) return e;
    }
    return std::nullopt;
}

/// One ClientReceiveChatMessages line as the client reads it.
struct FriendChat {
    std::string message, from;
    uint64_t    category = 0;
};

static std::optional<FriendChat> friend_chat_of(const fake::Delivered& d, const schema::ClassSchema& pc) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks || blocks->size() != 1) return std::nullopt;
    size_t pos = (*blocks)[0].payload_pos;
    const auto calls = repl::read_rpc_fields(pc, d.content.data(), pos + (*blocks)[0].payload_bits, pos);
    if (!calls || calls->size() != 1) return std::nullopt;
    const repl::RpcCall& call = (*calls)[0];
    if (!call.decoded || std::string(call.rpc->name) != "ClientReceiveChatMessages") return std::nullopt;
    const repl::Value* messages = repl::rpcf::value_of(*call.rpc, call.params, "Messages");
    const auto* list = messages ? messages->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.size() != 1) return std::nullopt;
    const repl::ElementState& e = list->elements.front();
    const repl::Value* m = repl::rpcf::element_value(e, 1);
    const repl::Value* f = repl::rpcf::element_value(e, 2);
    const repl::Value* c = repl::rpcf::element_value(e, 3);
    const std::string* mt = m ? m->get_if<std::string>() : nullptr;
    const std::string* ft = f ? f->get_if<std::string>() : nullptr;
    const uint64_t* ct = c ? c->get_if<uint64_t>() : nullptr;
    if (!mt || !ft || !ct) return std::nullopt;
    return FriendChat{*mt, *ft, *ct};
}

/// One ClientCheckIfCharacterNameExistsResponse as the client reads it.
struct FriendCheck {
    std::string name;
    bool        exists = false;
};

static std::optional<FriendCheck> friend_check_of(const fake::Delivered& d, const schema::ClassSchema& pc) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks || blocks->size() != 1) return std::nullopt;
    size_t pos = (*blocks)[0].payload_pos;
    const auto calls = repl::read_rpc_fields(pc, d.content.data(), pos + (*blocks)[0].payload_bits, pos);
    if (!calls || calls->size() != 1) return std::nullopt;
    const repl::RpcCall& call = (*calls)[0];
    if (!call.decoded || std::string(call.rpc->name) != "ClientCheckIfCharacterNameExistsResponse") return std::nullopt;
    const repl::Value* n = repl::rpcf::value_of(*call.rpc, call.params, "CharacterNameToCheck");
    const repl::Value* e = repl::rpcf::value_of(*call.rpc, call.params, "bDoesCharacterExist");
    const std::string* nt = n ? n->get_if<std::string>() : nullptr;
    const bool* et = e ? e->get_if<bool>() : nullptr;
    if (!nt) return std::nullopt;
    return FriendCheck{*nt, et ? *et : false};
}

/// The character-service side of the tests and the session's sink: the by-name and by-id lookups the friend calls ask, the progress the
/// session hands over, and the two lists it saves for a character that has no session here.  The lookup and save methods have the
/// signatures the session's PositionSink gains; on the old source they simply are never called (the tests are red there).
struct FriendService : PositionSink {
    struct Save {
        std::string                     id;
        std::vector<svc::Friend>        friends;
        std::vector<svc::FriendRequest> requests;
    };
    std::map<std::string, svc::Character> characters;   // id -> the character service's record
    std::vector<std::string>              looked_up;    // the names the session asked, in order
    std::vector<Save>                     saves;        // every save_friend_lists call, in order
    std::vector<std::pair<std::string, svc::Progress>> progress;
    bool                                  down = false;   // the service answers nothing
    std::chrono::milliseconds             lookup_delay{0};   // a slow character service (the name-check test)

    void save(const std::string&, const svc::Position&) override {}
    void save_progress(const std::string& id, svc::Progress p) override { progress.emplace_back(id, std::move(p)); }

    svc::CharacterResult character_by_name(const std::string& name) {
        if (lookup_delay.count() > 0) std::this_thread::sleep_for(lookup_delay);
        looked_up.push_back(name);
        svc::CharacterResult r;
        if (down) return r;
        for (const auto& [id, c] : characters) {
            if (c.name.size() != name.size()) continue;
            bool same = true;
            for (size_t i = 0; i < name.size(); ++i) {
                const char x = c.name[i] >= 'A' && c.name[i] <= 'Z' ? static_cast<char>(c.name[i] - 'A' + 'a') : c.name[i];
                const char y = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] - 'A' + 'a') : name[i];
                same = same && x == y;
            }
            if (!same) continue;
            r.status = svc::CallStatus::Ok;
            r.character = c;
            return r;
        }
        r.status = svc::CallStatus::NotFound;
        return r;
    }

    svc::CharacterResult character_by_id(const std::string& id) {
        svc::CharacterResult r;
        if (down) return r;
        const auto it = characters.find(id);
        if (it == characters.end()) {
            r.status = svc::CallStatus::NotFound;
            return r;
        }
        r.status = svc::CallStatus::Ok;
        r.character = it->second;
        return r;
    }

    void save_friend_lists(const std::string& id, std::vector<svc::Friend> friends, std::vector<svc::FriendRequest> requests) {
        auto it = characters.find(id);
        if (it != characters.end()) {
            it->second.friends = friends;
            it->second.friend_requests = requests;
        }
        saves.push_back({id, std::move(friends), std::move(requests)});
    }
};

/// Two or three sessions on one world, each with its own connection and client and ticked together.
struct FriendWorld {
    FriendService            service;   // before the world: its lookup worker may still hold a job while the world ends
    Fixture                  fx;
    Clock::time_point        t = t0;
    struct P {
        Connection                 conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
        fake::Client               client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
        std::unique_ptr<PlayerSession> s;
        std::vector<fake::Delivered>   got;
        std::string                    error;
    };
    std::array<P, 3> p;

    FriendWorld() { fx.world->set_position_sink(&service); }
    ~FriendWorld() {
        for (P& pl : p) {
            if (pl.s) pl.s->finish("the friend test ends");
        }
    }
    static const schema::ClassSchema& pc_schema() { return class_named("AoCPlayerControllerBP_C"); }
    P& operator[](size_t i) { return p[i]; }
    const PlayerSession& session(size_t i) const { return *p[i].s; }

    bool join(size_t i, const svc::Character& rec) {
        service.characters[rec.id] = rec;
        p[i].s = PlayerSession::join(p[i].conn, *fx.world, rec, identity(), world_time_at(t), p[i].error);
        return p[i].s != nullptr;
    }
    void take(P& pl, const fake::Received& r) {
        for (const auto& d : r.delivered) pl.got.push_back(d);
    }
    void flush(P& pl) {
        for (auto& pkt : pl.conn.flush(t)) take(pl, pl.client.receive(pkt));
    }
    void client_sends(P& pl, std::vector<fake::Bunch> bunches) {
        const auto pkt = pl.client.packet(std::move(bunches));
        const std::vector<InBunch> in = pl.conn.receive(pkt, t);
        pl.s->take_acks(pl.conn);
        for (const InBunch& b : in) pl.s->on_bunch(pl.conn, b, t);
    }
    void step() {
        t += milliseconds(20);
        for (P& pl : p) {
            if (!pl.s) continue;
            pl.s->tick(pl.conn, t);
            flush(pl);
            pl.s->take_acks(pl.conn);
        }
        for (P& pl : p) {
            if (!pl.s) continue;
            client_sends(pl, {});
        }
    }
    size_t total_got() const {
        size_t n = 0;
        for (const P& pl : p) n += pl.got.size();
        return n;
    }
    size_t pending() const {
        size_t n = 0;
        for (const P& pl : p) {
            if (pl.s) n += pl.s->join_pending() + pl.s->replies_pending();
        }
        return n;
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 4 && i < 800; ++i) {
            const size_t before = total_got();
            step();
            quiet = total_got() == before && pending() == 0 ? quiet + 1 : 0;
        }
    }
    bool enter(size_t i, const svc::Character& rec) {
        if (!join(i, rec) || !p[i].s || p[i].s->failed()) {
            std::printf("  [FAIL] player %zu does not join: %s\n", i, p[i].error.c_str());
            return false;
        }
        settle();
        const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
        client_sends(p[i], {p[i].client.actor(p[i].s->channel_of(bs::Role::Pawn), actor_rpc(pawn, "ServerClientIsReadyToPhaseIn", {}), true)});
        settle();
        return true;
    }
    /// A client call on player `i`'s PlayerController, then everything that follows it.
    void request(size_t i, const BunchWriter& call) {
        client_sends(p[i], {p[i].client.actor(p[i].s->channel_of(bs::Role::PlayerController), call, true)});
        settle();
    }
};

/// A character of these tests: cyb's kit and appearance, the given name, id and class (the class id decides in character setup).
static svc::Character friend_character(const std::string& name, const std::string& id, int32_t class_id, const char* class_name) {
    svc::Character c = cyb_record();
    c.id = id;
    c.name = name;
    c.class_id = class_id;
    c.class_name = class_name;
    c.archetype_id = 0;
    return c;
}

/// A friend entry of the store: a 32-hex GUID that is not the character's, a name, level, class and subclass, a guild, the still-friend flag.
static svc::Friend friend_entry(int n, bool still = true) {
    char guid[33];
    std::snprintf(guid, sizeof guid, "%032x", 0x10000000u + static_cast<unsigned>(n));
    svc::Friend f;
    f.guid = guid;
    f.name = "Friend" + std::to_string(n);
    f.level = 1 + n % 25;
    f.class_id = 1 + n % 8;
    f.sub_archetype = 0;
    f.guild_name = n % 3 == 0 ? "Sandal Makers" : std::string();
    f.still_friend = still;
    return f;
}

/// A pending request of the store for `recipient`: sender `n`, a name, the recipient's id, an expiry far in the future.
static svc::FriendRequest friend_request(int n, const std::string& recipient) {
    char guid[33];
    std::snprintf(guid, sizeof guid, "%032x", 0x20000000u + static_cast<unsigned>(n));
    svc::FriendRequest q;
    q.sender_guid = guid;
    q.sender_name = "Sender" + std::to_string(n);
    q.recipient_guid = recipient;
    q.expires_at_ms = kFriendLiveExpiryMs;
    return q;
}

/// A client ServerSendFriendRequest(RecipientCharacterName).
static BunchWriter friend_send_call(const std::string& name) {
    const schema::ClassSchema& pc = FriendWorld::pc_schema();
    const schema::RpcSchema* rpc = pc.rpc("ServerSendFriendRequest");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "RecipientCharacterName", repl::Value::str(name))) return BunchWriter{};
    return actor_rpc(pc, "ServerSendFriendRequest", params);
}

/// A client call of the friends that carries one character name (accept, decline).
static BunchWriter friend_name_call(const char* rpc_name, const std::string& name) {
    const schema::ClassSchema& pc = FriendWorld::pc_schema();
    const schema::RpcSchema* rpc = pc.rpc(rpc_name);
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "SenderCharacterName", repl::Value::str(name))) return BunchWriter{};
    return actor_rpc(pc, rpc_name, params);
}

/// A client ServerCheckIfCharacterNameExistsRequest(CharacterNameToCheck).
static BunchWriter friend_check_call(const std::string& name) {
    const schema::ClassSchema& pc = FriendWorld::pc_schema();
    const schema::RpcSchema* rpc = pc.rpc("ServerCheckIfCharacterNameExistsRequest");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "CharacterNameToCheck", repl::Value::str(name))) return BunchWriter{};
    return actor_rpc(pc, "ServerCheckIfCharacterNameExistsRequest", params);
}

/// A client ServerRemoveFriend(FriendToRemoveGuid).
static BunchWriter friend_remove_call(const std::string& hex) {
    const schema::ClassSchema& pc = FriendWorld::pc_schema();
    const schema::RpcSchema* rpc = pc.rpc("ServerRemoveFriend");
    const friend_guid g = friend_words(hex);
    repl::RpcParams params;
    bool ok = rpc != nullptr;
    const char* letters[4] = {".A", ".B", ".C", ".D"};
    for (int i = 0; ok && i < 4; ++i) {
        ok = repl::set_rpc_param(*rpc, params, std::string("FriendToRemoveGuid") + letters[i], repl::Value::i64(static_cast<int32_t>(g[static_cast<size_t>(i)])));
    }
    if (!ok) return BunchWriter{};
    return actor_rpc(pc, "ServerRemoveFriend", params);
}

/// The SocialComponent blocks player `i` was given from `from` on.
static std::vector<FriendBlock> friend_blocks(const FriendWorld& w, size_t i, size_t from) {
    return friend_blocks_of(*w.p[i].s, w.p[i].got, from);
}

/// True when the block changed the named array by exactly `changed` new or changed items and `deleted` deletions.
static bool friend_one_change(const FriendBlock& b, const char* array, size_t changed, size_t deleted) {
    const repl::FastArrayField* f = friend_field(b, array);
    return f && f->delta.changed.size() == changed && f->delta.deleted.size() == deleted;
}

/// A captured log, for the lines a refusal writes.
struct FriendLog {
    std::ostringstream              text;
    std::shared_ptr<spdlog::logger> previous;
    FriendLog() {
        previous = spdlog::default_logger();
        auto logger = std::make_shared<spdlog::logger>("friend-tests", std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);
    }
    ~FriendLog() { spdlog::set_default_logger(previous); }
    std::string str() const { return text.str(); }
};

}  // namespace

// ---- the export -------------------------------------------------------------------------------------------------------------

static void test_the_social_component_is_exported_with_the_characters_lists_in_the_first_block() {
    std::printf("the SocialComponent is exported after the quest and tutorial components with the character's lists in its first block\n");
    FriendWorld w;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {friend_entry(1), friend_entry(2, /*still=*/false)};
    a.friend_requests = {friend_request(3, kFriendA)};
    a.friend_requests.push_back(friend_request(4, kFriendA));
    a.friend_requests.back().expires_at_ms = friend_now_ms() - 1000;   // (stale: the login leaves it out)
    if (!w.enter(0, a)) {
        CHECK(false, "the character with 2 friends and 1 pending request joins");
        return;
    }
    const repl::Subobject* social = friend_social_of(*w.p[0].s);
    CHECK(social != nullptr, "the PlayerController carries a SocialComponent subobject");
    if (!social) return;
    CHECK(social->checksum == 0x0c79e4fb, "its export carries the class checksum 0x0c79e4fb");
    CHECK(std::string(social->name) == "SocialComponent" && social->obj.schema && std::string(social->obj.schema->name) == "UIntrepidSocialComponent",
          "it is exported as SocialComponent with the generated UIntrepidSocialComponent layout");
    size_t tracker_at = 0, quest_at = 0, ftue_at = 0, social_at = 0;
    bool tracker = false, quest = false, ftue = false;
    const auto& subs = w.p[0].s->bootstrap().player_controller().subobjects;
    for (size_t i = 0; i < subs.size(); ++i) {
        if (subs[i].name == "XPTracker") { tracker = true; tracker_at = i; }
        if (subs[i].name == "QuestConsumerComponent") { quest = true; quest_at = i; }
        if (subs[i].name == "FTUE Component") { ftue = true; ftue_at = i; }
        if (subs[i].name == "SocialComponent") social_at = i;
    }
    CHECK(tracker && quest && ftue && quest_at < social_at && ftue_at < social_at && tracker_at < social_at,
          "it comes after the XPTracker, the quest component and the tutorial component");
    bool exported = false, exported_once = true;
    std::string exported_at;
    for (size_t i = 0; i < w.p[0].got.size(); ++i) {
        const auto e = friend_social_export(w.p[0].got[i], *social);
        if (!e) continue;
        if (exported) exported_once = false;
        exported = true;
        exported_at += (exported_at.empty() ? "" : ", ") + std::to_string(i) + (w.p[0].got[i].open ? "(open)" : "");
        CHECK(e->flags == 7 && e->path == "SocialComponent" && e->checksum == 0x0c79e4fb && e->outer.ObjectId == w.p[0].s->bootstrap().setup().guids.player_controller.ObjectId,
              "its export is flags 7, the name SocialComponent, the checksum and the PlayerController as outer");
    }
    std::printf("    the SocialComponent's export went in chain(s) %s\n", exported_at.c_str());
    CHECK(exported && exported_once, "the SocialComponent is exported once in the join");
    const auto blocks = friend_blocks(w, 0, 0);
    CHECK(blocks.size() == 1 && blocks[0].payload_bits > 0, "the join carries one SocialComponent block with the character's lists");
    if (blocks.size() != 1) return;
    const auto* friends = friend_field(blocks[0], "FriendsList");
    const auto* pending = friend_field(blocks[0], "PendingFriendRequests");
    CHECK(friends && friends->delta.base_key == 0 && friends->delta.changed.size() == 2 && friends->delta.deleted.empty(),
          "the friends are whole in the first block (base key 0, 2 changed)");
    CHECK(pending && pending->delta.base_key == 0 && pending->delta.changed.size() == 1, "and the pending request (1 changed: the stale one is dropped at the login)");
    if (!friends || friends->delta.changed.size() != 2 || !pending || pending->delta.changed.size() != 1) return;
    FriendItem first, second, request;
    const bool read = friend_item_of(friends->delta.changed[0].values, false, first) && friend_item_of(friends->delta.changed[1].values, false, second) &&
                      friend_item_of(pending->delta.changed[0].values, true, request);
    CHECK(read, "every item reads back with the generated layout, field by field");
    if (!read) return;
    const friend_guid ga = friend_words(a.friends[0].guid), gb = friend_words(a.friends[1].guid), gq = friend_words(a.friend_requests[0].sender_guid);
    CHECK(first.guid == ga && first.name == a.friends[0].name && first.level == a.friends[0].level && first.class_id == static_cast<uint64_t>(a.friends[0].class_id) &&
              first.sub_archetype == 0 && first.guild == a.friends[0].guild_name && first.online == false && first.last_online == 0 && first.still_friend == true,
          "friend 0: its GUID, name, level, class, subclass 0, guild, offline, last online 0, still a friend");
    CHECK(second.guid == gb && second.name == a.friends[1].name && second.still_friend == false,
          "friend 1 keeps the one-way removal's still-friend flag false, in the store's order");
    CHECK(request.sender_guid == gq && request.sender_name == a.friend_requests[0].sender_name && request.recipient_guid == friend_words(kFriendA) &&
              request.expires == kFriendLiveExpiryMs / 1000,
          "the pending request: sender GUID and name, the recipient's GUID, the expiry in Unix seconds");
    std::printf("    the first block: %zu payload bits in a %zu-bit block\n", blocks[0].payload_bits, blocks[0].block_bits);
    for (const fake::Delivered& d : w.p[0].got) {
        if (!friend_block_of(d, *social)) continue;
        std::printf("    the first block (%zu bits): %s\n", blocks[0].block_bits, friend_bits_hex(d.content, blocks[0].start_bits, blocks[0].block_bits).c_str());
        break;
    }
}

AOC_REGISTER_TEST(test_the_social_component_is_exported_with_the_characters_lists_in_the_first_block);

static void test_a_character_with_no_friends_sends_the_export_and_an_empty_block() {
    std::printf("a character with no friends sends the SocialComponent's export and one 139-bit block with no payload\n");
    FriendWorld w;
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard"))) {
        CHECK(false, "the character with no friends joins");
        return;
    }
    const repl::Subobject* social = friend_social_of(*w.p[0].s);
    CHECK(social != nullptr, "the SocialComponent is on the PlayerController");
    if (!social) return;
    size_t blocks = 0;
    for (size_t i = 0; i < w.p[0].got.size(); ++i) {
        if (const auto e = friend_social_export(w.p[0].got[i], *social)) {
            CHECK(e->flags == 7 && e->path == "SocialComponent" && e->checksum == 0x0c79e4fb, "its export is the one new export: name, flags 7, checksum");
        }
        if (auto b = friend_block_of(w.p[0].got[i], *social)) {
            ++blocks;
            CHECK(b->payload_bits == 0 && !b->has_rep && b->fields.empty(), "its content block has no payload (no RepLayout block and no fast-array field)");
            CHECK(b->block_bits == 139, "the empty block is 139 bits: 1 bHasRepLayout + 1 bIsActor + 128 GUID + 1 stably named + 8 payload bits");
            std::printf("    the empty block (%zu bits): %s\n", b->block_bits, friend_bits_hex(w.p[0].got[i].content, b->start_bits, b->block_bits).c_str());
        }
    }
    CHECK(blocks == 1, "a character with no friends gets exactly one SocialComponent block");
}

AOC_REGISTER_TEST(test_a_character_with_no_friends_sends_the_export_and_an_empty_block);

static void test_a_character_with_100_friends_sends_them_whole_in_order_in_the_first_block() {
    std::printf("100 friends go in the first block, whole and in order, in one chain that fits the connection's limits\n");
    FriendWorld w;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    for (int i = 0; i < 100; ++i) a.friends.push_back(friend_entry(i));
    if (!w.enter(0, a)) {
        CHECK(false, "the character with 100 friends joins");
        return;
    }
    const auto blocks = friend_blocks(w, 0, 0);
    CHECK(blocks.size() == 1, "one SocialComponent block holds the first state");
    if (blocks.size() != 1) return;
    const auto* friends = friend_field(blocks[0], "FriendsList");
    CHECK(friends && friends->delta.base_key == 0 && friends->delta.changed.size() == 100, "100 friends, base key 0");
    if (!friends || friends->delta.changed.size() != 100) return;
    bool order = true, fields = true;
    for (size_t i = 0; i < friends->delta.changed.size(); ++i) {
        FriendItem item;
        if (!friend_item_of(friends->delta.changed[i].values, false, item)) {
            fields = false;
            break;
        }
        order = order && item.guid == friend_words(a.friends[i].guid) && item.name == a.friends[i].name;
        fields = fields && item.level == a.friends[i].level && item.class_id == static_cast<uint64_t>(a.friends[i].class_id) && item.sub_archetype == 0 &&
                 item.guild == a.friends[i].guild_name && item.last_online == 0 && !item.online && item.still_friend;
    }
    CHECK(order, "the 100 items are the character's list in its order, oldest first");
    CHECK(fields, "every one carries its GUID, name, level, class, subclass 0, guild, offline, last online 0 and still a friend");
    // the chain's own parts: its export bunches plus its content split at the connection's bunch size
    size_t chain_parts = 0;
    for (const fake::Delivered& d : w.p[0].got) {
        if (!friend_block_of(d, *friend_social_of(*w.p[0].s))) continue;
        chain_parts = d.exports.size() + (d.content.bit_pos() <= Connection::kMaxSingleBunchBits ? 1 : (d.content.bit_pos() + 7615) / 7616);
    }
    CHECK(blocks[0].payload_bits > 0 && chain_parts > 0 && chain_parts <= Connection::kMaxUnackedReliable,
          "the 100-friend block goes out in one chain of at most 256 reliable parts");
    std::printf("    the first block: %zu payload bits in a %zu-bit block, %zu chain parts\n", blocks[0].payload_bits, blocks[0].block_bits, chain_parts);
    for (const fake::Delivered& d : w.p[0].got) {
        if (!friend_block_of(d, *friend_social_of(*w.p[0].s))) continue;
        std::printf("    the first block (%zu bits, the first 128 bytes): %s\n", blocks[0].block_bits,
                    friend_bits_hex(d.content, blocks[0].start_bits, std::min<size_t>(1024, blocks[0].block_bits)).c_str());
        break;
    }
}

AOC_REGISTER_TEST(test_a_character_with_100_friends_sends_them_whole_in_order_in_the_first_block);

// ---- the calls -------------------------------------------------------------------------------------------------------------

static void test_a_friend_request_to_a_known_offline_character_is_stored_and_shown_at_the_next_login() {
    std::printf("a request to a known offline character is stored for it and shown in its first block at its next login\n");
    FriendWorld w;
    svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard"))) {
        CHECK(false, "the sender joins");
        return;
    }
    w.service.characters[kFriendB] = b;
    const size_t a_before = w.p[0].got.size();
    const int64_t before_ms = friend_now_ms();
    w.request(0, friend_send_call("Brannoc"));
    CHECK(w.p[0].s->friend_requests().empty() && w.p[0].s->friends().empty(), "the sender keeps no request and gets no friend entry");
    CHECK(w.service.saves.size() == 1 && w.service.saves[0].id == kFriendB && w.service.saves[0].friends.empty() && w.service.saves[0].requests.size() == 1,
          "the request is stored for the target, whose friends stay empty");
    const svc::FriendRequest saved = w.service.saves.empty() ? svc::FriendRequest{} : w.service.saves[0].requests[0];
    CHECK(saved.sender_guid == kFriendA && saved.sender_name == "Astra" && saved.recipient_guid == kFriendB &&
              saved.expires_at_ms >= before_ms + kFriendRequestLifetimeMs && saved.expires_at_ms <= friend_now_ms() + kFriendRequestLifetimeMs,
          "the stored request names the sender, the recipient and an expiry 14 days from now (milliseconds)");
    CHECK(friend_blocks(w, 0, a_before).empty(), "nothing is sent to the sender");
    if (!w.enter(1, w.service.characters[kFriendB])) {
        CHECK(false, "the target logs in");
        return;
    }
    CHECK(w.p[1].s->friend_requests().size() == 1, "the target's session holds the stored request");
    const auto blocks = friend_blocks(w, 1, 0);
    CHECK(blocks.size() == 1, "its first block carries it");
    if (blocks.size() != 1) return;
    const auto* pending = friend_field(blocks[0], "PendingFriendRequests");
    FriendItem item;
    CHECK(pending && pending->delta.changed.size() == 1 && friend_item_of(pending->delta.changed[0].values, true, item), "one pending request item with every field");
    CHECK(item.complete && item.sender_guid == friend_words(kFriendA) && item.sender_name == "Astra" && item.recipient_guid == friend_words(kFriendB) &&
              item.expires == saved.expires_at_ms / 1000,
          "the sender's GUID and name, the recipient's GUID and the expiry in Unix seconds");
    CHECK(w.p[1].s->friends().empty(), "no friend entry until it is accepted");
}

AOC_REGISTER_TEST(test_a_friend_request_to_a_known_offline_character_is_stored_and_shown_at_the_next_login);

static void test_unknown_self_and_ignoring_targets_follow_the_owners_rules() {
    std::printf("an unknown name, the character itself and a target that ignores the sender\n");
    FriendWorld w;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    b.ignored_character_names = {"Astra"};
    if (!w.enter(0, a) || !w.enter(1, b)) {
        CHECK(false, "both characters join");
        return;
    }
    const schema::ClassSchema& pc = FriendWorld::pc_schema();
    {
        FriendLog log;
        const size_t before = w.p[0].got.size();
        w.request(0, friend_send_call("Nobody_xyz"));
        std::vector<FriendChat> lines;
        for (size_t i = before; i < w.p[0].got.size(); ++i) {
            if (auto l = friend_chat_of(w.p[0].got[i], pc)) lines.push_back(std::move(*l));
        }
        CHECK(lines.size() == 1 && lines[0].message == "Nobody_xyz does not exist." && lines[0].from == "Server" && lines[0].category == 124,
              "an unknown name gets one system line \"<name> does not exist.\"");
        CHECK(w.p[0].s->friend_requests().empty() && w.service.saves.empty(), "and no pending entry");
        CHECK(log.str().find("Nobody_xyz") != std::string::npos, "the refusal is logged");
    }
    {
        FriendLog log;
        const size_t before = w.p[0].got.size();
        w.request(0, friend_send_call("Astra"));
        CHECK(w.p[0].s->friend_requests().empty() && w.p[1].s->friend_requests().empty() && w.service.saves.empty(),
              "a request to the character itself is refused with no pending entry");
        CHECK(friend_blocks(w, 0, before).empty(), "and sends nothing");
        CHECK(log.str().find("itself") != std::string::npos || log.str().find("own name") != std::string::npos, "the self request is logged");
    }
    {
        const size_t before_b = w.p[1].got.size();
        w.request(0, friend_send_call("Brannoc"));
        CHECK(w.p[1].s->friend_requests().size() == 1 && w.p[1].s->friend_requests()[0].sender_guid == kFriendA,
              "a request to a character that ignores the sender is still sent");
        const auto blocks = friend_blocks(w, 1, before_b);
        CHECK(blocks.size() == 1 && friend_one_change(blocks[0], "PendingFriendRequests", 1, 0), "the target is shown one new pending request");
    }
}

AOC_REGISTER_TEST(test_unknown_self_and_ignoring_targets_follow_the_owners_rules);

static void test_an_online_request_is_pending_until_it_is_accepted() {
    std::printf("a request to an online character is pending for it alone until it accepts, then each side gets one new friend\n");
    FriendWorld w;
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard")) || !w.enter(1, friend_character("Brannoc", kFriendB, 7, "Tank"))) {
        CHECK(false, "both characters join");
        return;
    }
    const size_t a_before = w.p[0].got.size(), b_before = w.p[1].got.size();
    w.request(0, friend_send_call("Brannoc"));
    CHECK(w.p[0].s->friends().empty() && w.p[0].s->friend_requests().empty(), "the sender gets no friend and no pending entry");
    CHECK(w.p[1].s->friend_requests().size() == 1 && w.p[1].s->friend_requests()[0].sender_guid == kFriendA, "the target has one pending request from the sender");
    CHECK(friend_blocks(w, 0, a_before).empty(), "the sender is sent nothing");
    const auto pending_blocks = friend_blocks(w, 1, b_before);
    CHECK(pending_blocks.size() == 1 && friend_one_change(pending_blocks[0], "PendingFriendRequests", 1, 0),
          "the target gets one fast-array change with exactly the pending item");
    const size_t a_accept = w.p[0].got.size(), b_accept = w.p[1].got.size();
    w.request(1, friend_name_call("ServerAcceptFriendRequest", "Astra"));
    CHECK(w.p[0].s->friends().size() == 1 && w.p[1].s->friends().size() == 1, "both sides are friends");
    if (w.p[0].s->friends().empty() || w.p[1].s->friends().empty()) return;
    CHECK(w.p[0].s->friends()[0].guid == kFriendB && w.p[1].s->friends()[0].guid == kFriendA, "each lists the other");
    CHECK(w.p[1].s->friend_requests().empty(), "the accept clears the target's pending request");
    const auto a_blocks = friend_blocks(w, 0, a_accept), b_blocks = friend_blocks(w, 1, b_accept);
    CHECK(a_blocks.size() == 1 && friend_one_change(a_blocks[0], "FriendsList", 1, 0) && friend_field(a_blocks[0], "PendingFriendRequests") == nullptr,
          "the sender gets one change: one new friend item, no pending field");
    CHECK(b_blocks.size() == 1 && friend_one_change(b_blocks[0], "FriendsList", 1, 0) && friend_one_change(b_blocks[0], "PendingFriendRequests", 0, 1),
          "the target gets one change: one new friend item and the pending item deleted");
    const repl::FastArrayField* af = a_blocks.empty() ? nullptr : friend_field(a_blocks[0], "FriendsList");
    FriendItem item;
    CHECK(af && af->delta.changed.size() == 1 && friend_item_of(af->delta.changed[0].values, false, item) && item.complete,
          "the new friend item decodes field by field");
    CHECK(item.guid == friend_words(kFriendB) && item.name == "Brannoc" && item.level == 1 && item.class_id == 7 && item.sub_archetype == 0 && item.guild.empty() &&
              item.online && item.last_online == 0 && item.still_friend,
          "it is the other character: class 7, subclass 0, no guild, online, last online 0, still a friend");
}

AOC_REGISTER_TEST(test_an_online_request_is_pending_until_it_is_accepted);

static void test_a_declined_request_is_cleared_with_no_friend() {
    std::printf("a declined request is cleared and leaves no friend on either side\n");
    FriendWorld w;
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard")) || !w.enter(1, friend_character("Brannoc", kFriendB, 7, "Tank"))) {
        CHECK(false, "both characters join");
        return;
    }
    w.request(0, friend_send_call("Brannoc"));
    const size_t b_before = w.p[1].got.size();
    w.request(1, friend_name_call("ServerDeclineFriendRequest", "Astra"));
    CHECK(w.p[1].s->friend_requests().empty(), "the target's pending request is gone");
    CHECK(w.p[0].s->friends().empty() && w.p[1].s->friends().empty(), "neither side has a friend");
    const auto blocks = friend_blocks(w, 1, b_before);
    CHECK(blocks.size() == 1 && friend_one_change(blocks[0], "PendingFriendRequests", 0, 1) && friend_field(blocks[0], "FriendsList") == nullptr,
          "one change: the pending item deleted, no friend item");
}

AOC_REGISTER_TEST(test_a_declined_request_is_cleared_with_no_friend);

static void test_two_requests_at_once_make_the_two_characters_friends_and_clear_both_pending_lists() {
    std::printf("two characters who request each other become friends, and both pending lists are cleared\n");
    FriendWorld w;
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard")) || !w.enter(1, friend_character("Brannoc", kFriendB, 7, "Tank"))) {
        CHECK(false, "both characters join");
        return;
    }
    w.request(0, friend_send_call("Brannoc"));
    CHECK(w.p[1].s->friend_requests().size() == 1, "the first request is pending on the target");
    const size_t a_before = w.p[0].got.size(), b_before = w.p[1].got.size();
    w.request(1, friend_send_call("Astra"));
    CHECK(w.p[0].s->friends().size() == 1 && w.p[1].s->friends().size() == 1, "the second request makes both friends");
    if (w.p[0].s->friends().empty() || w.p[1].s->friends().empty()) return;
    CHECK(w.p[0].s->friends()[0].guid == kFriendB && w.p[1].s->friends()[0].guid == kFriendA, "each lists the other");
    CHECK(w.p[0].s->friend_requests().empty() && w.p[1].s->friend_requests().empty(), "both pending lists are empty");
    const auto a_blocks = friend_blocks(w, 0, a_before), b_blocks = friend_blocks(w, 1, b_before);
    CHECK(a_blocks.size() == 1 && friend_one_change(a_blocks[0], "FriendsList", 1, 0), "the recipient of the second request gets one new friend item");
    CHECK(b_blocks.size() == 1 && friend_one_change(b_blocks[0], "FriendsList", 1, 0) && friend_one_change(b_blocks[0], "PendingFriendRequests", 0, 1),
          "the second sender gets one new friend item and its pending item deleted");
}

AOC_REGISTER_TEST(test_two_requests_at_once_make_the_two_characters_friends_and_clear_both_pending_lists);

static void test_a_one_way_removal_keeps_the_other_side_with_the_still_friend_flag_cleared() {
    std::printf("a removal is one-way: the remover's entry is deleted and the other side keeps it with still-friend false\n");
    FriendWorld w;
    svc::Friend entry_for_b = friend_entry(0);
    entry_for_b.guid = kFriendB;
    entry_for_b.name = "Brannoc";
    entry_for_b.level = 1;
    entry_for_b.class_id = 7;
    entry_for_b.guild_name.clear();
    svc::Friend entry_for_a = entry_for_b;
    entry_for_a.guid = kFriendA;
    entry_for_a.name = "Astra";
    entry_for_a.class_id = 2;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {entry_for_b};
    svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    b.friends = {entry_for_a};
    if (!w.enter(0, a) || !w.enter(1, b)) {
        CHECK(false, "both characters with each other as a friend join");
        return;
    }
    CHECK(w.p[0].s->friends().size() == 1 && w.p[0].s->friends()[0].still_friend, "the other side starts with still friend true");
    const size_t a_before = w.p[0].got.size(), b_before = w.p[1].got.size();
    w.request(1, friend_remove_call(kFriendA));
    CHECK(w.p[1].s->friends().empty(), "the remover's entry is deleted");
    CHECK(w.p[0].s->friends().size() == 1 && w.p[0].s->friends()[0].guid == kFriendB && !w.p[0].s->friends()[0].still_friend,
          "the other side keeps its entry with still friend false");
    const auto a_blocks = friend_blocks(w, 0, a_before), b_blocks = friend_blocks(w, 1, b_before);
    CHECK(b_blocks.size() == 1 && friend_one_change(b_blocks[0], "FriendsList", 0, 1), "the remover gets one change: the entry deleted");
    CHECK(a_blocks.size() == 1 && friend_one_change(a_blocks[0], "FriendsList", 1, 0), "the other side gets one change: its entry, still friend false");
    const repl::FastArrayField* changed = a_blocks.empty() ? nullptr : friend_field(a_blocks[0], "FriendsList");
    FriendItem item;
    CHECK(changed && changed->delta.changed.size() == 1 && friend_item_of(changed->delta.changed[0].values, false, item) && item.guid == friend_words(kFriendB) &&
              !item.still_friend,
          "the changed item is the other character's entry, field by field, still friend false");
    const size_t a_again = w.p[0].got.size(), b_again = w.p[1].got.size();
    w.request(0, friend_remove_call(kFriendB));
    CHECK(w.p[0].s->friends().empty(), "the other side's own removal deletes its entry");
    CHECK(friend_blocks(w, 0, a_again).size() == 1 && friend_blocks(w, 1, b_again).empty(), "only the remover is sent a change (the other side has no entry any more)");
}

AOC_REGISTER_TEST(test_a_one_way_removal_keeps_the_other_side_with_the_still_friend_flag_cleared);

static void test_a_removal_and_an_accept_that_do_not_apply_are_refused_silently_with_the_list_unchanged() {
    std::printf("a removal or an accept that does not apply sends nothing and is logged only\n");
    FriendWorld w;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {friend_entry(0)};
    if (!w.enter(0, a)) {
        CHECK(false, "the character joins");
        return;
    }
    FriendLog log;
    const size_t before = w.p[0].got.size();
    const auto friends_before = w.p[0].s->friends();
    w.request(0, friend_remove_call(kFriendB));
    CHECK(w.p[0].s->friends() == friends_before, "removing a character that is not on the list leaves it as it is");
    w.request(0, friend_name_call("ServerAcceptFriendRequest", "Nobody_xyz"));
    CHECK(w.p[0].s->friends() == friends_before && w.p[0].s->friend_requests().empty(), "accepting a request that does not exist leaves both lists as they are");
    w.request(0, friend_name_call("ServerDeclineFriendRequest", "Nobody_xyz"));
    CHECK(w.p[0].s->friend_requests().empty(), "declining a request that does not exist leaves the list as it is");
    CHECK(friend_blocks(w, 0, before).empty(), "no change is sent for any of them");
    CHECK(log.str().find("not on the list") != std::string::npos && log.str().find("no request") != std::string::npos, "each refusal is logged");
}

AOC_REGISTER_TEST(test_a_removal_and_an_accept_that_do_not_apply_are_refused_silently_with_the_list_unchanged);

static void test_a_hundred_friends_are_the_most_and_the_one_after_is_refused() {
    std::printf("100 friends are the most: an accept and a request that would make 101 are refused and logged\n");
    FriendWorld w;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    for (int i = 0; i < 99; ++i) a.friends.push_back(friend_entry(i));
    svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    if (!w.enter(0, a) || !w.enter(1, b)) {
        CHECK(false, "both characters join");
        return;
    }
    w.request(1, friend_send_call("Astra"));
    w.request(0, friend_name_call("ServerAcceptFriendRequest", "Brannoc"));
    CHECK(w.p[0].s->friends().size() == 100 && w.p[1].s->friends().size() == 1, "the accept brings the list to 100");
    svc::Character c = friend_character("Cass", kFriendC, 3, "Cleric");
    if (!w.enter(2, c)) {
        CHECK(false, "the third character joins");
        return;
    }
    w.request(2, friend_send_call("Astra"));
    {
        FriendLog log;
        const auto before = w.p[0].s->friends();
        w.request(0, friend_name_call("ServerAcceptFriendRequest", "Cass"));
        CHECK(w.p[0].s->friends() == before && w.p[0].s->friends().size() == 100, "the accept that would make 101 is refused and the list stays at 100");
        CHECK(log.str().find("refused") != std::string::npos, "the refusal is logged");
    }
    {
        FriendLog log;
        const auto before = w.p[0].s->friends();
        w.request(2, friend_send_call("Astra"));
        w.request(0, friend_send_call("Cass"));
        CHECK(w.p[0].s->friends() == before && w.p[0].s->friends().size() == 100 && w.p[2].s->friends().empty(),
              "a request whose completion would make 101 is refused: neither side gets a friend");
        CHECK(log.str().find("refused") != std::string::npos, "the refusal is logged");
    }
}

AOC_REGISTER_TEST(test_a_hundred_friends_are_the_most_and_the_one_after_is_refused);

static void test_a_hundred_pending_requests_are_the_most_and_the_one_after_is_refused() {
    std::printf("100 pending requests are the most: the request after them is refused and logged\n");
    FriendWorld w;
    svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    for (int i = 0; i < 100; ++i) b.friend_requests.push_back(friend_request(i, kFriendB));
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard"))) {
        CHECK(false, "the sender joins");
        return;
    }
    w.service.characters[kFriendB] = b;
    FriendLog log;
    const size_t before = w.service.saves.size();
    w.request(0, friend_send_call("Brannoc"));
    CHECK(w.service.saves.size() == before && w.service.characters[kFriendB].friend_requests.size() == 100,
          "the target's 100 pending requests are not changed");
    CHECK(w.p[0].s->friend_requests().empty(), "the sender gets no pending entry either");
    CHECK(log.str().find("refused") != std::string::npos, "the refusal is logged");
}

AOC_REGISTER_TEST(test_a_hundred_pending_requests_are_the_most_and_the_one_after_is_refused);

// ---- the online state and the updates --------------------------------------------------------------------------------------

static void test_a_friends_login_and_logout_flip_the_online_flag_on_every_list_that_names_him() {
    std::printf("a friend's login and logout flip the online flag on every list that names him\n");
    FriendWorld w;
    svc::Friend b_in_a, b_in_c;
    b_in_a.guid = kFriendB;
    b_in_a.name = "Brannoc";
    b_in_a.level = 1;
    b_in_a.class_id = 7;
    b_in_c = b_in_a;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {b_in_a};
    svc::Character c = friend_character("Cass", kFriendC, 3, "Cleric");
    c.friends = {b_in_c};
    if (!w.enter(0, a) || !w.enter(2, c)) {
        CHECK(false, "the two owners join");
        return;
    }
    const size_t a_before = w.p[0].got.size(), c_before = w.p[2].got.size();
    if (!w.enter(1, friend_character("Brannoc", kFriendB, 7, "Tank"))) {
        CHECK(false, "the friend joins");
        return;
    }
    const auto a_online = friend_blocks(w, 0, a_before), c_online = friend_blocks(w, 2, c_before);
    CHECK(a_online.size() == 1 && friend_one_change(a_online[0], "FriendsList", 1, 0), "the first owner gets one changed item for the login");
    CHECK(c_online.size() == 1 && friend_one_change(c_online[0], "FriendsList", 1, 0), "the second owner gets one changed item too");
    for (const auto* blocks : {&a_online, &c_online}) {
        const repl::FastArrayField* f = blocks->empty() ? nullptr : friend_field((*blocks)[0], "FriendsList");
        FriendItem item;
        CHECK(f && f->delta.changed.size() == 1 && friend_item_of(f->delta.changed[0].values, false, item) && item.online,
              "the changed item is the friend, online true");
    }
    const size_t a_off = w.p[0].got.size(), c_off = w.p[2].got.size();
    w.p[1].s->finish("the friend logs out");
    w.settle();
    const auto a_offline = friend_blocks(w, 0, a_off), c_offline = friend_blocks(w, 2, c_off);
    CHECK(a_offline.size() == 1 && friend_one_change(a_offline[0], "FriendsList", 1, 0), "the first owner gets one changed item for the logout");
    CHECK(c_offline.size() == 1 && friend_one_change(c_offline[0], "FriendsList", 1, 0), "the second owner too");
    for (const auto* blocks : {&a_offline, &c_offline}) {
        const repl::FastArrayField* f = blocks->empty() ? nullptr : friend_field((*blocks)[0], "FriendsList");
        FriendItem item;
        CHECK(f && f->delta.changed.size() == 1 && friend_item_of(f->delta.changed[0].values, false, item) && !item.online,
              "the changed item is the friend, online false");
    }
}

AOC_REGISTER_TEST(test_a_friends_login_and_logout_flip_the_online_flag_on_every_list_that_names_him);

static void test_an_untouched_list_sends_nothing_again_and_a_login_sends_one_item() {
    std::printf("an untouched list sends nothing again; the friend's login sends one item\n");
    FriendWorld w;
    svc::Friend friend_b;
    friend_b.guid = kFriendB;
    friend_b.name = "Brannoc";
    friend_b.level = 1;
    friend_b.class_id = 7;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {friend_b};
    if (!w.enter(0, a)) {
        CHECK(false, "the owner joins");
        return;
    }
    CHECK(friend_blocks(w, 0, 0).size() == 1, "the join carried the first block");
    const size_t before = w.p[0].got.size();
    for (int i = 0; i < 25; ++i) w.step();
    CHECK(friend_blocks(w, 0, before).empty(), "25 ticks of an untouched list send no SocialComponent change");
}

AOC_REGISTER_TEST(test_an_untouched_list_sends_nothing_again_and_a_login_sends_one_item);

static void test_a_login_elsewhere_keeps_the_reverse_index_and_the_online_flag_right() {
    std::printf("a login elsewhere (the old session ends, the lists go to the new one) keeps the online flag right\n");
    FriendWorld w;
    svc::Friend b_in_a;
    b_in_a.guid = kFriendB;
    b_in_a.name = "Brannoc";
    b_in_a.level = 1;
    b_in_a.class_id = 7;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {b_in_a};
    if (!w.enter(0, a)) {
        CHECK(false, "the owner joins");
        return;
    }
    const svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    if (!w.enter(1, b)) {
        CHECK(false, "the friend joins");
        return;
    }
    CHECK(w.p[1].s->friends() == b.friends, "the friend's session holds its lists");
    const size_t a_online = w.p[0].got.size();
    std::vector<svc::Friend> carried_friends = w.p[1].s->friends();
    std::vector<svc::FriendRequest> carried_requests = w.p[1].s->friend_requests();
    w.p[1].s->finish("a login elsewhere");
    w.p[1].s.reset();
    w.settle();
    CHECK(friend_blocks(w, 0, a_online).size() == 1, "the old session's end tells the owner the friend is offline");
    svc::Character b2 = b;
    b2.friends = carried_friends;
    b2.friend_requests = carried_requests;
    w.service.characters[kFriendB] = b2;
    const size_t a_again = w.p[0].got.size();
    if (!w.enter(1, b2)) {
        CHECK(false, "the character joins again elsewhere");
        return;
    }
    const auto blocks = friend_blocks(w, 0, a_again);
    CHECK(blocks.size() == 1 && friend_one_change(blocks[0], "FriendsList", 1, 0), "the owner gets the new session's login as one changed item");
    const repl::FastArrayField* f = blocks.empty() ? nullptr : friend_field(blocks[0], "FriendsList");
    FriendItem item;
    CHECK(f && f->delta.changed.size() == 1 && friend_item_of(f->delta.changed[0].values, false, item) && item.online,
          "the item is the friend, online true from the new session");
    const size_t a_end = w.p[0].got.size();
    w.p[1].s->finish("the second session ends");
    w.settle();
    CHECK(friend_blocks(w, 0, a_end).size() == 1, "the second session's end tells the owner once more");
}

AOC_REGISTER_TEST(test_a_login_elsewhere_keeps_the_reverse_index_and_the_online_flag_right);

static void test_the_name_check_is_answered_at_once_with_the_by_name_lookup() {
    std::printf("the name check is answered with the by-name lookup\n");
    FriendWorld w;
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard")) || !w.enter(1, friend_character("Brannoc", kFriendB, 7, "Tank"))) {
        CHECK(false, "both characters join");
        return;
    }
    const schema::ClassSchema& pc = FriendWorld::pc_schema();
    const auto answer = [&](const std::string& name) {
        const size_t before = w.p[0].got.size();
        w.request(0, friend_check_call(name));
        // (the lookup runs on the worker: wait for its answer, a later tick brings it)
        for (int i = 0; i < 1000; ++i) {
            for (size_t k = before; k < w.p[0].got.size(); ++k) {
                if (auto a = friend_check_of(w.p[0].got[k], pc)) return *a;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            w.step();
        }
        return FriendCheck{};
    };
    const FriendCheck known = answer("Brannoc");
    CHECK(known.name == "Brannoc" && known.exists, "a known name is answered true, with the name the client sent");
    const FriendCheck unknown = answer("Nobody_xyz");
    CHECK(unknown.name == "Nobody_xyz" && !unknown.exists, "an unknown name is answered false");
    CHECK(w.service.looked_up.size() == 2 && w.service.looked_up[0] == "Brannoc", "each answer is the by-name lookup, at once");
    CHECK(w.p[0].s->friend_requests().empty() && w.p[0].s->friends().empty(), "the check changes no list");
}

AOC_REGISTER_TEST(test_the_name_check_is_answered_at_once_with_the_by_name_lookup);

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  session/player_friends.inc  (social, step 7: the friends list)
//
//  The friends and the pending friend requests a character has.  The character service keeps them (migration 0024); worldd reads them with the
//  character at the login (GetCharacter) and saves them whole with the progress (SaveProgress with replace_friends and replace_friend_requests).
//  The PlayerController's SocialComponent carries them to the client: the first block with the lists whole, oldest first, and then one fast-array
//  change per change.
//
//  What a call does (the fixed rules of 2026-11-09).
//    request   a name that is not a character is refused with one SYSBROADCAST line "<name> does not exist." and no pending entry; the character's
//              own name is refused (logged, no line); a character who ignores the sender still gets the request.
//              A request to a character with a session in this world goes on that session's pending list; one to an offline character is saved for
//              it and delivered at its next login.  A request that meets the target's own request to the sender (both requested at once) completes
//              the friendship: both pending entries are cleared and each side gets the other as a friend.
//    accept    the request is taken off the pending list, the two are friends on both sides (the other side is its session here, or its record
//              saved).  An accept with no such request, or one that would make a list 101 friends, changes nothing and is logged.
//    decline   the request is taken off; no friend.
//    remove    one-way: the remover's entry is deleted, the other side's entry stays with still_friend false until it removes the character too.
//              A removal of a character that is not on the list changes nothing and is logged.
//    check     ServerCheckIfCharacterNameExistsRequest is answered at once with the by-name lookup (ClientCheckIfCharacterNameExistsResponse).
//
//  The online flag is not stored: a friend is online when a session of this world has the character now (the chat directory's rule, phase-in done).
//  A session keeps a reverse index from each friend's GUID to the sessions whose FriendsList names it; the phase-in tells them the character is
//  online and the end of the session tells them it is offline, and the entry moves with the list.  The last online time is not stored yet: 0.
//  A change is handed over with the progress at once (and once more at the end of the session); the expiry is seconds on the wire, the store's
//  milliseconds divided by 1,000.
// ============================================================================

namespace {

/// The lifetime of a new request: 14 days in the store's milliseconds (the fixed rule and the game data's PendingFriendRequestTimeInterval).
constexpr int64_t kFriendRequestLifetimeMs = 14ll * 24 * 60 * 60 * 1000;

/// The chat category of a server line (the welcome line's and the party's).
constexpr int kFriendSysBroadcast = 124;

/// The letters A to Z folded to lower case; every other byte is as it is (as the chat directory and the ignore list compare names).
char friend_fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool friend_same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return friend_fold(x) == friend_fold(y); });
}

/// True when two 32-hex-digit GUIDs are the same (the letters' case does not count).
bool friend_same_guid_text(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return friend_fold(x) == friend_fold(y); });
}

/// The four words of a 32-hex-digit GUID; all zero for a value that is not one (the store's records are valid).
std::array<uint32_t, 4> friend_words(const std::string& hex) {
    const auto words = bootstrap::character_guid_words(hex);
    return words ? *words : std::array<uint32_t, 4>{};
}

/// The 32 lower-case hex digits of four GUID words (the form the store keeps).
std::string friend_hex(const std::array<uint32_t, 4>& words) {
    char text[33];
    std::snprintf(text, sizeof text, "%08x%08x%08x%08x", words[0], words[1], words[2], words[3]);
    return text;
}

/// A player's text as it may go into the log: a control character becomes '?', and a long text is cut.
std::string friend_shown(const std::string& text) {
    std::string out = text.substr(0, 80);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
    }
    if (text.size() > 80) out += "...";
    return out;
}

/// The name a call carries at `path`; nullopt when the call does not decode, an empty string when the string is not on the wire.
std::optional<std::string> friend_name_in(const repl::ClientRpc& rpc, const char* path) {
    if (!rpc.call.decoded) return std::nullopt;
    const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path);
    const std::string* text = v ? v->get_if<std::string>() : nullptr;
    return text ? *text : std::string();
}

/// The four int32 words of the FGuid at `prefix` of a client call; nullopt when the call does not decode.
std::optional<std::array<uint32_t, 4>> friend_guid_in(const repl::ClientRpc& rpc, const char* prefix) {
    if (!rpc.call.decoded) return std::nullopt;
    std::array<uint32_t, 4> words{};
    for (int i = 0; i < 4; ++i) {
        const std::string path = std::string(prefix) + "." + "ABCD"[i];
        const auto word = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path), 0);
        if (!word) return std::nullopt;
        words[static_cast<size_t>(i)] = static_cast<uint32_t>(*word);
    }
    return words;
}

/// The reverse index: each friend's character GUID to the sessions whose FriendsList names it, for the online flag's changes.  One process's worlds at
/// once (a session is told only when it is in the same world); the sessions keep it right (session/player_friends.inc, friends_index_refresh).
std::map<std::array<uint32_t, 4>, std::vector<PlayerSession*>>& friend_index() {
    static std::map<std::array<uint32_t, 4>, std::vector<PlayerSession*>> index;
    return index;
}

/// The entry of `list` for the character `guid`, or the end of the list.
std::vector<svc::Friend>::iterator friend_find(std::vector<svc::Friend>& list, const std::string& guid) {
    return std::find_if(list.begin(), list.end(), [&](const svc::Friend& f) { return friend_same_guid_text(f.guid, guid); });
}

/// The entry of `list` for the pending request whose sender is `guid`, or the end of the list.
std::vector<svc::FriendRequest>::iterator friend_find_request(std::vector<svc::FriendRequest>& list, const std::string& guid) {
    return std::find_if(list.begin(), list.end(), [&](const svc::FriendRequest& q) { return friend_same_guid_text(q.sender_guid, guid); });
}

/// The entry of `list` for the pending request whose sender has this name, or the end of the list.
std::vector<svc::FriendRequest>::iterator friend_find_request_name(std::vector<svc::FriendRequest>& list, const std::string& name) {
    return std::find_if(list.begin(), list.end(), [&](const svc::FriendRequest& q) { return friend_same_name(q.sender_name, name); });
}

/// True when `list` has the character `guid` as a friend now; an entry a one-way removal left with still_friend false does not count (finding 5).
bool friend_is_friend(const std::vector<svc::Friend>& list, const std::string& guid) {
    return std::any_of(list.begin(), list.end(), [&](const svc::Friend& f) { return f.still_friend && friend_same_guid_text(f.guid, guid); });
}

/// Puts `entry` on `list`: at the end, or in the place of a one-way removal's entry (still_friend false), which counts as a friend again.  False when the
/// character is a friend already (nothing changes).
bool friend_add(std::vector<svc::Friend>& list, const svc::Friend& entry) {
    const auto at = friend_find(list, entry.guid);
    if (at == list.end()) {
        list.push_back(entry);
        return true;
    }
    if (at->still_friend) return false;
    *at = entry;   // (the same place: the list stays oldest first)
    at->still_friend = true;
    return true;
}

/// One friend entry for the character `side`: its own GUID, name, level, class and no subclass or guild (the class's and the game's defaults today).
svc::Friend friend_make(const std::string& id, const std::string& name, int32_t level, int32_t class_id) {
    svc::Friend f;
    f.guid = id;
    f.name = name;
    f.level = level;
    f.class_id = class_id;
    f.sub_archetype = 0;
    f.guild_name.clear();
    f.still_friend = true;
    return f;
}

}  // namespace

// ---- the join ---------------------------------------------------------------------------------------------------------------

void PlayerSession::init_friends(const svc::Character& character) {
    // The login read the record before this session was built; a change saved after that read (another character's friend call, the end of an older session)
    // is newer.  The sink's by-id read waits for the progress it still holds for the character, so this is the newest list at the join (finding 2).
    svc::Character newest;
    const svc::Character* source = &character;
    if (sink_) {
        // The saver may still hold a progress for the character that the login's read did not see: its lists are newer than both the read and the service,
        // and taking them waits for nothing.  Without one, the by-id read is the newest (it waits for a progress that is already in the worker).
        std::vector<svc::Friend> pending_friends;
        std::vector<svc::FriendRequest> pending_requests;
        if (sink_->pending_friends(character.id, pending_friends, pending_requests)) {
            newest = character;
            newest.friends = std::move(pending_friends);
            newest.friend_requests = std::move(pending_requests);
            source = &newest;
        } else {
            svc::CharacterResult found = sink_->character_by_id(character.id);
            if (found.status == svc::CallStatus::Ok && found.character.id == character.id) {
                newest = std::move(found.character);
                source = &newest;
            }
        }
    }
    const size_t saved_friends = source->friends.size();
    const size_t kept_friends = std::min(saved_friends, bootstrap::kMaxFriends);
    friends_.assign(source->friends.begin(), source->friends.begin() + static_cast<std::ptrdiff_t>(kept_friends));
    const int64_t now = wall_ms();
    size_t expired = 0;
    for (const svc::FriendRequest& request : source->friend_requests) {
        if (request.expires_at_ms <= now) {
            ++expired;
            continue;
        }
        if (friend_requests_.size() >= bootstrap::kMaxFriendRequests) continue;
        friend_requests_.push_back(request);
    }
    if (kept_friends < saved_friends) {
        spdlog::warn("[friends] {}: {} of the {} friends the character saved are left out (the list holds at most {})", pb_.setup().name, saved_friends - kept_friends,
                     saved_friends, bootstrap::kMaxFriends);
    }
    if (expired > 0) {
        spdlog::info("[friends] {}: {} pending friend request(s) of the character had expired and are left out", pb_.setup().name, expired);
    }
    const size_t requests = source->friend_requests.size();
    if (requests > friend_requests_.size() + expired) {
        spdlog::warn("[friends] {}: {} pending request(s) of the character are left out (the list holds at most {})", pb_.setup().name,
                     requests - friend_requests_.size() - expired, bootstrap::kMaxFriendRequests);
    }
    refresh_friend_wire();
    friends_index_refresh();
    if (!friends_.empty() || !friend_requests_.empty()) {
        spdlog::info("[friends] {}: {} friend(s) and {} pending request(s) on the character's lists", pb_.setup().name, friends_.size(), friend_requests_.size());
    }
}

// ---- the online state -------------------------------------------------------------------------------------------------------

PlayerSession* PlayerSession::friend_session_of(const std::array<uint32_t, 4>& guid) const {
    for (ChatEntry* e : ChatEntry::in_world(world_)) {
        if (e->self && e->self->pb_.setup().character_guid == guid) return e->self;
    }
    return nullptr;
}

bool PlayerSession::friend_online(const std::array<uint32_t, 4>& guid) const { return friend_session_of(guid) != nullptr; }

void PlayerSession::refresh_friend_wire() {
    std::vector<bootstrap::FriendWire> friends;
    friends.reserve(friends_.size());
    for (const svc::Friend& f : friends_) {
        bootstrap::FriendWire wire;
        wire.guid = friend_words(f.guid);
        wire.name = f.name;
        wire.level = f.level;
        wire.class_id = f.class_id;
        wire.sub_archetype = f.sub_archetype;
        wire.guild_name = f.guild_name;
        wire.online = friend_online(wire.guid);
        wire.last_online = 0;   // (not stored yet: the owner's pick)
        wire.still_friend = f.still_friend;
        friends.push_back(std::move(wire));
    }
    std::vector<bootstrap::FriendRequestWire> requests;
    requests.reserve(friend_requests_.size());
    for (const svc::FriendRequest& q : friend_requests_) {
        bootstrap::FriendRequestWire wire;
        wire.sender_guid = friend_words(q.sender_guid);
        wire.sender_name = q.sender_name;
        wire.recipient_guid = friend_words(q.recipient_guid);
        wire.expires = q.expires_at_ms / 1000;   // (the store's milliseconds, the wire's Unix seconds)
        requests.push_back(std::move(wire));
    }
    if (!pb_.set_friends(friends, requests)) {
        spdlog::warn("[friends] {}: the social component does not take the lists: {}", name(), pb_.error().empty() ? "no reason" : pb_.error());
    }
}

void PlayerSession::send_friend_update(Connection& conn) {
    refresh_friend_wire();
    queue("SocialComponent (friends)", [](bootstrap::PlayerBootstrap& pb) { return pb.friends_update(); }, conn, /*must_build=*/false);
}

void PlayerSession::friends_index_refresh() {
    auto& index = friend_index();
    for (const std::array<uint32_t, 4>& key : friend_index_keys_) {
        const auto it = index.find(key);
        if (it == index.end()) continue;
        it->second.erase(std::remove(it->second.begin(), it->second.end(), this), it->second.end());
        if (it->second.empty()) index.erase(key);
    }
    friend_index_keys_.clear();
    std::set<std::array<uint32_t, 4>> keys;
    for (const svc::Friend& f : friends_) {
        const std::array<uint32_t, 4> key = friend_words(f.guid);
        if (key != std::array<uint32_t, 4>{}) keys.insert(key);
    }
    for (const std::array<uint32_t, 4>& key : keys) {
        index[key].push_back(this);
        friend_index_keys_.push_back(key);
    }
}

void PlayerSession::announce_friends_online(bool online) {
    const auto it = friend_index().find(pb_.setup().character_guid);
    if (it == friend_index().end()) return;
    size_t told = 0;
    for (PlayerSession* owner : it->second) {
        if (!owner || owner == this || owner->world_ != world_ || owner->finished_ || owner->failed_) continue;
        Connection* conn = owner->chat_entry_.connection;
        if (!conn) continue;
        owner->send_friend_update(*conn);
        ++told;
    }
    spdlog::log(level_for(friend_logged_, 20), "[friends] {}: {} friend list(s) told the character is {}", name(), told, online ? "online" : "offline");
}

void PlayerSession::drain_friend_checks(Connection& conn) {
    while (!friend_checks_.empty()) {
        svc::CharacterResult found;
        if (!world_ || !world_->take_friend_lookup(friend_checks_.front().first, found)) return;   // (the oldest first: one worker answers in order)
        std::string name = std::move(friend_checks_.front().second);
        friend_checks_.pop_front();
        if (found.status != svc::CallStatus::Ok && found.status != svc::CallStatus::NotFound) {
            spdlog::warn("[friends] {}: the name check of \"{}\" has no answer: the character service says {}", this->name(), friend_shown(name), svc::call_status_name(found.status));
        }
        const bool exists = found.status == svc::CallStatus::Ok;
        queue("ClientCheckIfCharacterNameExistsResponse", [name, exists](bootstrap::PlayerBootstrap& pb) { return pb.name_check_answer(name, exists); }, conn,
              /*must_build=*/false);
    }
}

void PlayerSession::run_friends(Connection& conn, Clock::time_point now) {
    (void)now;
    drain_friend_checks(conn);   // (an answer the lookup worker brought back, whatever the phase-in)
    if (!phase_in_done_ || friends_online_announced_) return;
    friends_online_announced_ = true;
    // (a friend who phased in between the join's first block and now: the one item that changed, not a second first block)
    send_friend_update(conn);
    announce_friends_online(true);
}

void PlayerSession::leave_friends(const char* why) {
    (void)why;
    if (!friends_online_announced_) return;
    friends_online_announced_ = false;
    announce_friends_online(false);
}

void PlayerSession::forget_friends() {
    auto& index = friend_index();
    for (const std::array<uint32_t, 4>& key : friend_index_keys_) {
        const auto it = index.find(key);
        if (it == index.end()) continue;
        it->second.erase(std::remove(it->second.begin(), it->second.end(), this), it->second.end());
        if (it->second.empty()) index.erase(key);
    }
    friend_index_keys_.clear();
}

// ---- the two sides of a call ------------------------------------------------------------------------------------------------

bool PlayerSession::friend_load_self(FriendSide& side) {
    side.session = this;
    side.id = character_id_;
    side.name = name();
    side.level = pb_.setup().level;
    side.class_id = class_id_;
    side.friends = friends_;
    side.requests = friend_requests_;
    return true;
}

bool PlayerSession::friend_load_other(const std::array<uint32_t, 4>& guid, FriendSide& side) {
    PlayerSession* here = friend_session_of(guid);
    if (!here) {
        // a session that has joined but has not phased in is the character's side too: the change goes on its lists (and its next save keeps it) rather
        // than beside a session that would save over it (finding 2)
        if (ChatEntry* joining = ChatEntry::joined(world_, guid)) here = joining->self;
    }
    if (PlayerSession* other = here) {
        side.session = other;
        side.id = other->character_id_;
        side.name = other->name();
        side.level = other->pb_.setup().level;
        side.class_id = other->class_id_;
        side.friends = other->friends_;
        side.requests = other->friend_requests_;
        return true;
    }
    if (!sink_) return false;
    const svc::CharacterResult found = sink_->character_by_id(friend_hex(guid));
    if (found.status != svc::CallStatus::Ok) return false;
    side.session = nullptr;
    side.id = found.character.id.empty() ? friend_hex(guid) : found.character.id;
    side.name = found.character.name;
    side.level = found.character.level;
    side.class_id = found.character.class_id;
    side.friends = found.character.friends;
    side.requests = found.character.friend_requests;
    return true;
}

bool PlayerSession::friend_store(FriendSide& side, Connection* self_conn, const char* why) {
    if (PlayerSession* owner = side.session) {
        owner->friends_ = std::move(side.friends);
        owner->friend_requests_ = std::move(side.requests);
        owner->friends_index_refresh();
        if (owner->sink_) {
            if (owner->phase_in_done_) {
                owner->progress_changed_ = true;
                owner->hand_over_progress(why);
            } else {
                // A session that has not phased in still holds the progress of its login read, and that read may be older than the last progress of
                // the session this one replaces.  Only the two lists this call changed go to the saver: a lists-only save merges into a queued full
                // progress (its newer level, experience and the rest stay) and leaves a written one as it is.
                owner->sink_->save_friend_lists(owner->character_id_, owner->friends_, owner->friend_requests_);
            }
        }
        if (owner == this) {
            if (self_conn) send_friend_update(*self_conn);
        } else if (Connection* conn = owner->chat_entry_.connection) {
            owner->send_friend_update(*conn);
        }
        return true;
    }
    if (!sink_) return false;
    sink_->save_friend_lists(side.id, std::move(side.friends), std::move(side.requests));
    return true;
}

// ---- the calls --------------------------------------------------------------------------------------------------------------

void PlayerSession::on_send_friend_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    const std::optional<std::string> sent = friend_name_in(rpc, "RecipientCharacterName");
    if (!sent) {
        if (++friend_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[friends] {}: a ServerSendFriendRequest does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    const std::string target_name = *sent;
    if (target_name.empty()) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: a request with no name is refused", name());
        return;
    }
    if (friend_same_name(target_name, name())) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: a request to the character's own name is refused", name());
        return;
    }
    if (!sink_) {
        spdlog::warn("[friends] {}: a request by name needs the character service, and this session has none", name());
        return;
    }
    const svc::CharacterResult found = sink_->character_by_name(target_name);
    if (found.status == svc::CallStatus::NotFound) {
        if (!failed_) {
            // The fixed rule: one system line, whatever the reply queue holds.  A full queue does not drop it; the first over-limit line is logged (finding 10).
            const size_t waiting = replies_.size() + delayed_.size();
            if (waiting + 1 > kMaxPendingReplies && ++friend_lines_over_ == 1) {
                spdlog::warn("[friends] {}: the reply queue was full ({} steps): the unknown-name line is queued anyway", name(), waiting);
            }
            const std::string line = target_name + " does not exist.";
            queue("ClientReceiveChatMessages (friend request)", [line](bootstrap::PlayerBootstrap& pb) { return pb.chat_line(line, kFriendSysBroadcast, "Server"); }, conn,
                  /*must_build=*/false);
        }
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: a request to \"{}\" is refused: no character has that name", name(), friend_shown(target_name));
        return;
    }
    if (found.status != svc::CallStatus::Ok) {
        spdlog::warn("[friends] {}: a request to \"{}\" cannot be answered: the character service says {}", name(), friend_shown(target_name),
                     svc::call_status_name(found.status));
        return;
    }
    if (found.character.id.empty() || friend_same_guid_text(found.character.id, character_id_)) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: a request to its own character is refused", name());
        return;
    }
    const std::array<uint32_t, 4> key = friend_words(found.character.id);
    FriendSide mine;
    if (!friend_load_self(mine)) return;
    if (friend_is_friend(mine.friends, found.character.id)) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: a request to {} is refused: it is a friend already", name(), friend_shown(found.character.name));
        return;
    }
    FriendSide theirs;
    if (!friend_load_other(key, theirs)) {
        spdlog::warn("[friends] {}: a request to {} cannot be stored: its character is not here and the character service did not answer", name(), friend_shown(found.character.name));
        return;
    }
    // both requested at once: the sender's own pending list already holds the target's request
    const auto pending = friend_find_request(mine.requests, theirs.id);
    if (pending != mine.requests.end()) {
        // (an entry a one-way removal left with still_friend false is put back in its place, so it is no new friend)
        const bool mine_new = friend_find(mine.friends, theirs.id) == mine.friends.end();
        const bool theirs_new = friend_find(theirs.friends, mine.id) == theirs.friends.end();
        if ((mine_new && mine.friends.size() >= bootstrap::kMaxFriends) || (theirs_new && theirs.friends.size() >= bootstrap::kMaxFriends)) {
            spdlog::log(level_for(friend_refused_logged_), "[friends] {}: the request to {} is refused: the friendship would make a list 101 friends", name(),
                        friend_shown(theirs.name));
            return;
        }
        friend_add(mine.friends, friend_make(theirs.id, theirs.name, theirs.level, theirs.class_id));
        friend_add(theirs.friends, friend_make(mine.id, mine.name, mine.level, mine.class_id));
        mine.requests.erase(pending);
        if (const auto at = friend_find_request(theirs.requests, mine.id); at != theirs.requests.end()) theirs.requests.erase(at);
        const size_t count = mine.friends.size();
        if (!friend_store(theirs, &conn, "two friends requested each other")) return;
        if (!friend_store(mine, &conn, "two friends requested each other")) return;
        spdlog::log(level_for(friend_logged_, 20), "[friends] {}: the two characters requested each other: they are friends ({} friend(s))", name(), count);
        return;
    }
    if (friend_find_request(theirs.requests, mine.id) != theirs.requests.end()) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: the request to {} waits already", name(), friend_shown(theirs.name));
        return;
    }
    if (theirs.requests.size() >= bootstrap::kMaxFriendRequests) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: the request to {} is refused: its pending list holds {}, the most there can be", name(),
                    friend_shown(theirs.name), theirs.requests.size());
        return;
    }
    svc::FriendRequest request;
    request.sender_guid = mine.id;
    request.sender_name = mine.name;
    request.recipient_guid = theirs.id;
    request.expires_at_ms = wall_ms() + kFriendRequestLifetimeMs;
    theirs.requests.push_back(std::move(request));
    const size_t count = theirs.requests.size();
    if (!friend_store(theirs, &conn, "a friend request")) return;
    spdlog::log(level_for(friend_logged_, 20), "[friends] {}: a friend request to {} is stored ({} pending request(s))", name(), friend_shown(theirs.name), count);
}

void PlayerSession::on_accept_friend_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    const std::optional<std::string> sent = friend_name_in(rpc, "SenderCharacterName");
    if (!sent) {
        if (++friend_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[friends] {}: a ServerAcceptFriendRequest does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    FriendSide mine;
    if (!friend_load_self(mine)) return;
    const auto pending = friend_find_request_name(mine.requests, *sent);
    if (pending == mine.requests.end()) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: the accept of \"{}\" is refused: no request from that name waits", name(), friend_shown(*sent));
        return;
    }
    FriendSide theirs;
    if (!friend_load_other(friend_words(pending->sender_guid), theirs)) {
        spdlog::warn("[friends] {}: the accept of {} cannot be stored: its character is not here and the character service did not answer", name(), friend_shown(pending->sender_name));
        return;
    }
    if (friend_is_friend(mine.friends, theirs.id) || friend_is_friend(theirs.friends, mine.id)) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: the accept of {} is refused: the two are friends already", name(), friend_shown(theirs.name));
        return;
    }
    // (a still-friend-false entry from a one-way removal is restored in its place, not added again)
    const bool mine_new = friend_find(mine.friends, theirs.id) == mine.friends.end();
    const bool theirs_new = friend_find(theirs.friends, mine.id) == theirs.friends.end();
    if ((mine_new && mine.friends.size() >= bootstrap::kMaxFriends) || (theirs_new && theirs.friends.size() >= bootstrap::kMaxFriends)) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: the accept of {} is refused: the friendship would make a list 101 friends", name(), friend_shown(theirs.name));
        return;
    }
    friend_add(mine.friends, friend_make(theirs.id, theirs.name, theirs.level, theirs.class_id));
    friend_add(theirs.friends, friend_make(mine.id, mine.name, mine.level, mine.class_id));
    mine.requests.erase(pending);
    if (const auto at = friend_find_request(theirs.requests, mine.id); at != theirs.requests.end()) theirs.requests.erase(at);
    const size_t count = mine.friends.size();
    if (!friend_store(theirs, &conn, "a friend request accepted")) return;
    if (!friend_store(mine, &conn, "a friend request accepted")) return;
    spdlog::log(level_for(friend_logged_, 20), "[friends] {}: accepted {} ({} friend(s) on the list, at the end)", name(), friend_shown(theirs.name), count);
}

void PlayerSession::on_decline_friend_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    const std::optional<std::string> sent = friend_name_in(rpc, "SenderCharacterName");
    if (!sent) {
        if (++friend_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[friends] {}: a ServerDeclineFriendRequest does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    FriendSide mine;
    if (!friend_load_self(mine)) return;
    const auto pending = friend_find_request_name(mine.requests, *sent);
    if (pending == mine.requests.end()) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: the decline of \"{}\" is refused: no request from that name waits", name(), friend_shown(*sent));
        return;
    }
    const std::string whose = pending->sender_name;
    mine.requests.erase(pending);
    if (!friend_store(mine, &conn, "a friend request declined")) return;
    spdlog::log(level_for(friend_logged_, 20), "[friends] {}: declined the request of {} ({} pending request(s))", name(), friend_shown(whose), mine.requests.size());
}

void PlayerSession::on_remove_friend(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    const std::optional<std::array<uint32_t, 4>> guid = friend_guid_in(rpc, "FriendToRemoveGuid");
    if (!guid) {
        if (++friend_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[friends] {}: a ServerRemoveFriend does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    const std::string hex = friend_hex(*guid);
    FriendSide mine;
    if (!friend_load_self(mine)) return;
    const auto at = friend_find(mine.friends, hex);
    if (at == mine.friends.end()) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: the removal of {} is refused: it is not on the list", name(), hex);
        return;
    }
    // The other side first: when its record cannot be read nothing changes.  Removing the entry here and leaving still-friend true on the other side would
    // break the one-way rule with no way to repair it (finding 9).
    FriendSide theirs;
    if (!friend_load_other(*guid, theirs)) {
        spdlog::log(level_for(friend_refused_logged_), "[friends] {}: the removal of {} is refused: its character is not here and the character service did not answer",
                    name(), hex);
        return;
    }
    if (const auto other = friend_find(theirs.friends, mine.id); other != theirs.friends.end()) {
        other->still_friend = false;   // (one-way: the other side keeps the entry until it removes the character too)
        if (!friend_store(theirs, &conn, "a friend removed the character")) return;
    }
    mine.friends.erase(at);
    if (!friend_store(mine, &conn, "a friend removed")) return;
    spdlog::log(level_for(friend_logged_, 20), "[friends] {}: removed a friend ({} friend(s) on the list)", name(), mine.friends.size());
}

void PlayerSession::on_check_character_name(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    const std::optional<std::string> sent = friend_name_in(rpc, "CharacterNameToCheck");
    if (!sent) {
        if (++friend_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[friends] {}: a ServerCheckIfCharacterNameExistsRequest does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    bool exists = false;
    if (!sent->empty() && sink_) {
        if (world_) {
            // The lookup runs on the world's worker, off the network thread; drain_friend_checks queues the answer when it comes back (finding 6).  Only so
            // many checks wait at once: a client that sends more than 64 before the service answers loses the extra ones, and the first drop is logged.
            if (friend_checks_.size() >= 64) {
                if (++friend_checks_dropped_ == 1) {
                    spdlog::warn("[friends] {}: {} name checks wait for the character service: the further ones are dropped", name(), friend_checks_.size());
                }
                return;
            }
            friend_checks_.emplace_back(world_->submit_friend_lookup(*sent, /*by_name=*/true), *sent);
            return;
        }
        const svc::CharacterResult found = sink_->character_by_name(*sent);
        exists = found.status == svc::CallStatus::Ok;
        if (found.status != svc::CallStatus::Ok && found.status != svc::CallStatus::NotFound) {
            spdlog::warn("[friends] {}: the name check of \"{}\" has no answer: the character service says {}", name(), friend_shown(*sent), svc::call_status_name(found.status));
        }
    }
    queue("ClientCheckIfCharacterNameExistsResponse", [name = *sent, exists](bootstrap::PlayerBootstrap& pb) { return pb.name_check_answer(name, exists); }, conn,
          /*must_build=*/false);
}

void PlayerSession::register_friends_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerSendFriendRequest", [](PlayerSession& s, const RpcContext& c) { s.on_send_friend_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerAcceptFriendRequest", [](PlayerSession& s, const RpcContext& c) { s.on_accept_friend_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerDeclineFriendRequest", [](PlayerSession& s, const RpcContext& c) { s.on_decline_friend_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRemoveFriend", [](PlayerSession& s, const RpcContext& c) { s.on_remove_friend(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerCheckIfCharacterNameExistsRequest", [](PlayerSession& s, const RpcContext& c) { s.on_check_character_name(c.conn, c.rpc, c.now); });
}

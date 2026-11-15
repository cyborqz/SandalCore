// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  bootstrap/bootstrap_social.inc  (social, step 7: the friends list's component)
//
//  The PlayerController's SocialComponent (UIntrepidSocialComponent): the two FastArrays the client's friends window reads, FriendsList and
//  PendingFriendRequests.  The component is a default subobject the client knows by name, so its export is its name and the class checksum, and
//  its first content block is what gives it its id.  It goes after the quest and tutorial components, as the retail login's big chain does.
//
//  The first state is the character's lists whole, oldest first (the store's order).  After that, set_friends brings the arrays to the lists it is
//  given and friends_update() writes what differs: an add for a new friend or request, a change for one whose values differ (a friend's online flag
//  above all), a remove, all in one fast-array field.  An unchanged list writes nothing: the array key does not move, so the connection's shadow
//  still matches.  The friend's GUID is the item's identity (the pending request's sender GUID), so the items keep their ReplicationIDs.
// ============================================================================

namespace {

/// The four words of a friend's or sender's GUID as an item of the list holds them (the first four element commands).
std::optional<std::array<uint32_t, 4>> social_item_guid(const repl::ElementState& values) {
    std::array<uint32_t, 4> words{};
    for (uint16_t i = 1; i <= 4; ++i) {
        const auto it = values.values.find(i);
        const int64_t* v = it == values.values.end() ? nullptr : it->second.get_if<int64_t>();
        if (!v) return std::nullopt;
        words[i - 1] = static_cast<uint32_t>(*v);
    }
    return words;
}

/// One friend as the list's element commands carry it: every command by its path, so a schema whose order moved is refused rather than written wrong.
std::optional<repl::ElementState> social_friend_element(const repl::FastArrayLayout& layout, const FriendWire& f) {
    repl::ElementState es;
    for (const schema::RepCommand& c : layout.element) {
        const std::string_view path = c.path;
        const auto ends = [&](std::string_view suffix) {
            return path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        Value v;
        if (ends("SubArchetype")) {
            v = Value::u64(static_cast<uint64_t>(f.sub_archetype));
        } else if (ends("Archetype")) {
            v = Value::u64(static_cast<uint64_t>(f.class_id));
        } else if (ends("CharacterGuid.A")) {
            v = Value::i64(static_cast<int32_t>(f.guid[0]));
        } else if (ends("CharacterGuid.B")) {
            v = Value::i64(static_cast<int32_t>(f.guid[1]));
        } else if (ends("CharacterGuid.C")) {
            v = Value::i64(static_cast<int32_t>(f.guid[2]));
        } else if (ends("CharacterGuid.D")) {
            v = Value::i64(static_cast<int32_t>(f.guid[3]));
        } else if (ends("CharacterName")) {
            v = Value::str(f.name);
        } else if (ends("CharacterLevel")) {
            v = Value::i64(f.level);
        } else if (ends("GuildName")) {
            v = Value::str(f.guild_name);
        } else if (ends("bIsOnline")) {
            v = Value::boolean(f.online);
        } else if (ends("LastOnlineTimestamp")) {
            v = Value::i64(f.last_online);
        } else if (ends("bIsStillFriend")) {
            v = Value::boolean(f.still_friend);
        } else {
            return std::nullopt;
        }
        es.values[c.handle] = std::move(v);
    }
    if (!repl::element_fits(layout.element, layout.table, es)) return std::nullopt;
    return es;
}

/// One pending request as the list's element commands carry it.
std::optional<repl::ElementState> social_request_element(const repl::FastArrayLayout& layout, const FriendRequestWire& r) {
    repl::ElementState es;
    for (const schema::RepCommand& c : layout.element) {
        const std::string_view path = c.path;
        const auto ends = [&](std::string_view suffix) {
            return path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        Value v;
        if (ends("SenderCharacterGuid.A")) {
            v = Value::i64(static_cast<int32_t>(r.sender_guid[0]));
        } else if (ends("SenderCharacterGuid.B")) {
            v = Value::i64(static_cast<int32_t>(r.sender_guid[1]));
        } else if (ends("SenderCharacterGuid.C")) {
            v = Value::i64(static_cast<int32_t>(r.sender_guid[2]));
        } else if (ends("SenderCharacterGuid.D")) {
            v = Value::i64(static_cast<int32_t>(r.sender_guid[3]));
        } else if (ends("SenderCharacterName")) {
            v = Value::str(r.sender_name);
        } else if (ends("RecipientCharacterGuid.A")) {
            v = Value::i64(static_cast<int32_t>(r.recipient_guid[0]));
        } else if (ends("RecipientCharacterGuid.B")) {
            v = Value::i64(static_cast<int32_t>(r.recipient_guid[1]));
        } else if (ends("RecipientCharacterGuid.C")) {
            v = Value::i64(static_cast<int32_t>(r.recipient_guid[2]));
        } else if (ends("RecipientCharacterGuid.D")) {
            v = Value::i64(static_cast<int32_t>(r.recipient_guid[3]));
        } else if (ends("RequestExpiresTimestamp")) {
            v = Value::i64(r.expires);
        } else {
            return std::nullopt;
        }
        es.values[c.handle] = std::move(v);
    }
    if (!repl::element_fits(layout.element, layout.table, es)) return std::nullopt;
    return es;
}

}  // namespace

bool PlayerBootstrap::sync_social(repl::FastArrays& arrays) {
    const schema::ClassSchema* cls = gen::find_component_schema("SocialComponent");
    if (!cls) return fail("no schema for the social component (UIntrepidSocialComponent)");
    repl::FastArrayState* friends = arrays.get(*cls, "FriendsList");
    repl::FastArrayState* requests = arrays.get(*cls, "PendingFriendRequests");
    if (!friends || !requests) return fail("the social component has no FriendsList or PendingFriendRequests array");

    std::map<std::array<uint32_t, 4>, int32_t> friend_ids;
    for (const repl::FastArrayItem& item : friends->items()) {
        const auto g = social_item_guid(item.values);
        if (!g) return fail("a social component friend item does not read");
        friend_ids[*g] = item.id;
    }
    std::set<int32_t> kept;
    for (const FriendWire& f : friend_wire_) {
        auto element = social_friend_element(friends->layout(), f);
        if (!element) return fail("a friend does not fit the social component's list: " + f.name);
        const auto it = friend_ids.find(f.guid);
        if (it == friend_ids.end()) {
            if (!friends->add(std::move(*element))) return fail("the social component's list refused a friend: " + f.name);
            continue;
        }
        kept.insert(it->second);
        const repl::FastArrayItem* held = friends->find(it->second);
        if (!held) return fail("a social component friend item went missing: " + f.name);
        if (!(held->values == *element) && !friends->change(it->second, std::move(*element))) {
            return fail("the social component's list refused a changed friend: " + f.name);
        }
    }
    for (const auto& [guid, id] : friend_ids) {
        if (!kept.count(id)) friends->remove(id);
    }

    std::map<std::array<uint32_t, 4>, int32_t> request_ids;
    for (const repl::FastArrayItem& item : requests->items()) {
        const auto g = social_item_guid(item.values);
        if (!g) return fail("a social component pending request item does not read");
        request_ids[*g] = item.id;
    }
    kept.clear();
    for (const FriendRequestWire& r : friend_request_wire_) {
        auto element = social_request_element(requests->layout(), r);
        if (!element) return fail("a pending request does not fit the social component's list: " + r.sender_name);
        const auto it = request_ids.find(r.sender_guid);
        if (it == request_ids.end()) {
            if (!requests->add(std::move(*element))) return fail("the social component's list refused a pending request: " + r.sender_name);
            continue;
        }
        kept.insert(it->second);
        const repl::FastArrayItem* held = requests->find(it->second);
        if (!held) return fail("a social component pending request item went missing: " + r.sender_name);
        if (!(held->values == *element) && !requests->change(it->second, std::move(*element))) {
            return fail("the social component's list refused a changed pending request: " + r.sender_name);
        }
    }
    for (const auto& [guid, id] : request_ids) {
        if (!kept.count(id)) requests->remove(id);
    }
    return true;
}

bool PlayerBootstrap::set_friends(const std::vector<FriendWire>& friends, const std::vector<FriendRequestWire>& requests) {
    friend_wire_ = friends;
    friend_request_wire_ = requests;
    if (!ok_) return false;
    if (!social_added_) return sync_social(social_arrays_);
    repl::Subobject* social = pc_.find(setup_.guids.social);
    return social && sync_social(social->obj.arrays);
}

bool PlayerBootstrap::add_social_component() {
    repl::Subobject social = make_component(setup_.guids.social, "SocialComponent", kChecksumSocial);
    if (!social.obj.schema) return fail("no schema for the social component (UIntrepidSocialComponent)");
    social.obj.arrays = std::move(social_arrays_);
    social_arrays_ = repl::FastArrays{};
    pc_.subobjects.push_back(std::move(social));
    social_added_ = true;
    repl::Subobject* added = pc_.find(setup_.guids.social);
    return added && sync_social(added->obj.arrays);
}

std::optional<Send> PlayerBootstrap::friends_update() {
    if (!ok_ || !social_added_) return std::nullopt;
    return update(Role::PlayerController, "SocialComponent", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::name_check_answer(const std::string& name, bool exists) {
    if (!ok_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientCheckIfCharacterNameExistsResponse");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "CharacterNameToCheck", Value::str(name)) ||
        !repl::set_rpc_param(*rpc, params, "bDoesCharacterExist", Value::boolean(exists))) {
        return std::nullopt;
    }
    repl::RpcRequest call;
    call.rpc = "ClientCheckIfCharacterNameExistsResponse";
    call.params = std::move(params);
    return rpcs(Role::PlayerController, "ClientCheckIfCharacterNameExistsResponse", std::span<const repl::RpcRequest>(&call, 1));
}

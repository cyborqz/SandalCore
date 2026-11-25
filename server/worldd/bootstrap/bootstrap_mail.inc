// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/bootstrap_mail.inc  (mail: the mailbox's window, its list and its detail, the two claims, the
//  status marks, and the SocialComponent's unread count)
//
//  The six client calls the mail window makes: ClientOpenMailUI(MailboxGuid) on the PlayerController,
//  ClientUpdateMailHeaders(Headers, Version, bComplete) on the PlayerController (one chain per page), and
//  ClientGetMailData(MailData), ClientClaimMailItem(MailGuid, MailItemIndex), ClientClaimMailCurrency(MailGuid)
//  and ClientUpdateMailStatus(MailGuid, Status) on the PlayerController's SocialComponent (the last one call
//  per mail, all in one chain).  NumUnreadMail is the same component's plain property: the caller sets it and
//  sends the change with friends_update().
//
//  The page size and the Version/bComplete meaning are ours: the client's data fixes no page, and no mail call
//  has been recorded.  Version is the answer's generation (the client drops the pages it kept when a bigger one
//  arrives) and bComplete 1 on the last page makes the client take the list; both are caller values here.
// ============================================================================

namespace {

/// The SocialComponent's subobject, once the join's last chain has put it on the PlayerController; nullptr before that.
const repl::Subobject* mail_social(const PlayerBootstrap& pb) {
    if (!pb.ok()) return nullptr;
    return pb.player_controller().find(pb.setup().guids.social);
}

/// The guid's four words as the call names them (`prefix` + ".A" to ".D"), by path: a schema whose order moved is refused, not written wrong.
bool mail_guid_params(const schema::RpcSchema& rpc, repl::RpcParams& params, const char* prefix, const repl::GuidValue& guid) {
    const uint32_t words[4] = {guid.a, guid.b, guid.c, guid.d};
    const char* const suffix[4] = {".A", ".B", ".C", ".D"};
    for (size_t i = 0; i < 4; ++i) {
        const std::string path = std::string(prefix) + suffix[i];
        if (!repl::set_rpc_param(rpc, params, path, Value::i64(static_cast<int32_t>(words[i])))) return false;
    }
    return true;
}

}  // namespace

std::optional<Send> PlayerBootstrap::open_mail_ui(const repl::GuidValue& mailbox) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientOpenMailUI");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!mail_guid_params(*rpc, params, "MailboxGuid", mailbox)) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientOpenMailUI";
    call.params = std::move(params);
    return rpcs(Role::PlayerController, "ClientOpenMailUI", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<std::vector<Send>> PlayerBootstrap::mail_headers(std::span<const repl::MailHeader> headers, uint64_t version) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientUpdateMailHeaders");
    if (!rpc) return std::nullopt;
    const size_t pages = headers.empty() ? 1 : (headers.size() + kMaxMailHeadersPerPage - 1) / kMaxMailHeadersPerPage;
    std::vector<Send> out;
    out.reserve(pages);
    for (size_t page = 0; page < pages; ++page) {
        const size_t from = page * kMaxMailHeadersPerPage;
        const size_t n = std::min(kMaxMailHeadersPerPage, headers.size() - from);
        repl::ArrayValue list;
        list.elements.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            repl::ElementState element;
            element.values[1] = Value(headers[from + i]);
            list.elements.push_back(std::move(element));
        }
        repl::RpcParams params;
        if (!repl::set_rpc_param(*rpc, params, "Headers", Value(std::move(list))) ||
            !repl::set_rpc_param(*rpc, params, "Version", Value::u64(version)) ||
            !repl::set_rpc_param(*rpc, params, "bComplete", Value::boolean(page + 1 == pages))) {
            return std::nullopt;
        }
        repl::RpcRequest call;
        call.rpc = "ClientUpdateMailHeaders";
        call.params = std::move(params);
        auto send = rpcs(Role::PlayerController, "ClientUpdateMailHeaders", std::span<const repl::RpcRequest>(&call, 1));
        if (!send) return std::nullopt;
        out.push_back(std::move(*send));
    }
    return out;
}

std::optional<Send> PlayerBootstrap::mail_data(const repl::MailPersistenceData& mail) {
    if (!ok_ || !social_added_) return std::nullopt;
    const repl::Subobject* social = mail_social(*this);
    if (!social || !social->obj.schema) return std::nullopt;
    const schema::RpcSchema* rpc = social->obj.schema->rpc("ClientGetMailData");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "MailData", Value(mail))) return std::nullopt;
    return finish(Role::PlayerController, false, "ClientGetMailData", repl::call_rpc(pc_, social, "ClientGetMailData", params, {}, acks_));
}

std::optional<Send> PlayerBootstrap::mail_item_claimed(const repl::GuidValue& mail, int32_t item_index) {
    if (!ok_ || !social_added_) return std::nullopt;
    const repl::Subobject* social = mail_social(*this);
    if (!social || !social->obj.schema) return std::nullopt;
    const schema::RpcSchema* rpc = social->obj.schema->rpc("ClientClaimMailItem");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!mail_guid_params(*rpc, params, "MailGuid", mail) || !repl::set_rpc_param(*rpc, params, "MailItemIndex", Value::i64(item_index))) return std::nullopt;
    return finish(Role::PlayerController, false, "ClientClaimMailItem", repl::call_rpc(pc_, social, "ClientClaimMailItem", params, {}, acks_));
}

std::optional<Send> PlayerBootstrap::mail_currency_claimed(const repl::GuidValue& mail) {
    if (!ok_ || !social_added_) return std::nullopt;
    const repl::Subobject* social = mail_social(*this);
    if (!social || !social->obj.schema) return std::nullopt;
    const schema::RpcSchema* rpc = social->obj.schema->rpc("ClientClaimMailCurrency");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!mail_guid_params(*rpc, params, "MailGuid", mail)) return std::nullopt;
    return finish(Role::PlayerController, false, "ClientClaimMailCurrency", repl::call_rpc(pc_, social, "ClientClaimMailCurrency", params, {}, acks_));
}

std::optional<Send> PlayerBootstrap::mail_status_update(std::span<const repl::GuidValue> mails, uint32_t status) {
    if (!ok_ || !social_added_ || mails.empty() || mails.size() > kMaxMailStatusUpdates) return std::nullopt;
    const repl::Subobject* social = mail_social(*this);
    if (!social || !social->obj.schema) return std::nullopt;
    const schema::RpcSchema* rpc = social->obj.schema->rpc("ClientUpdateMailStatus");
    if (!rpc) return std::nullopt;
    std::vector<repl::RpcRequest> calls;
    calls.reserve(mails.size());
    for (const repl::GuidValue& mail : mails) {
        repl::RpcParams params;
        if (!mail_guid_params(*rpc, params, "MailGuid", mail) || !repl::set_rpc_param(*rpc, params, "Status", Value::u64(status))) return std::nullopt;
        repl::RpcRequest call;
        call.target = social;
        call.rpc = "ClientUpdateMailStatus";
        call.params = std::move(params);
        calls.push_back(std::move(call));
    }
    return finish(Role::PlayerController, false, "ClientUpdateMailStatus", repl::call_rpcs(pc_, calls, acks_));
}

bool PlayerBootstrap::set_num_unread_mail(int32_t count) {
    if (!ok_ || !social_added_ || count < 0) return false;
    repl::Subobject* social = pc_.find(setup_.guids.social);
    if (!social || !social->obj.schema) return false;
    return social->obj.state.set(*social->obj.schema, "NumUnreadMail", Value::i64(count));
}

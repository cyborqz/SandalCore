// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/mail_codec.inc  (included once by codecs.cpp, at the repl namespace's level)
//
//  See mail_codec.h for the layouts.  These are the definitions of the mail NetSerialize codecs; they
//  use codecs.cpp's private count, guid and unique-key helpers and the FString, FLocalizedMessage and
//  FItemInstance codecs.
// ============================================================================

/// A UBool: 32 bits, 0 or 1 (a value above 1 is an archive error).
static std::optional<bool> read_mail_ubool(wire::PacketReader& r) {
    const uint32_t v = r.read_uint32();
    if (overran(r) || v > 1) return std::nullopt;
    return v != 0;
}

static void write_mail_ubool(emit::BunchWriter& out, bool b) { out.write_uint32(b ? 1u : 0u); }

// ---- FMailHeader -------------------------------------------------------------

bool mail_header_fits(const MailHeader& h) {
    return fstring_fits(h.sender) && fstring_fits(h.subject) && localized_message_fits(h.localized_subject);
}

std::optional<MailHeader> read_mail_header(wire::PacketReader& r) {
    MailHeader h;
    h.guid = read_guid(r);
    auto sender = read_fstring(r);
    auto subject = read_fstring(r);
    auto localized = read_localized_message(r);
    auto attachments = read_mail_ubool(r);
    if (!sender || !subject || !localized || !attachments) return std::nullopt;
    h.sender = std::move(*sender);
    h.subject = std::move(*subject);
    h.localized_subject = std::move(*localized);
    h.has_attachments = *attachments;
    h.status = static_cast<int32_t>(r.read_uint32());
    h.expiration_time = static_cast<int64_t>(r.read_uint64());
    h.issue_time = static_cast<int64_t>(r.read_uint64());
    if (overran(r)) return std::nullopt;
    return h;
}
// C-O-A and C_Y_B

bool write_mail_header(emit::BunchWriter& out, const MailHeader& h) {
    if (!mail_header_fits(h)) return false;
    write_guid(out, h.guid);
    if (!write_fstring(out, h.sender) || !write_fstring(out, h.subject) || !write_localized_message(out, h.localized_subject)) return false;
    write_mail_ubool(out, h.has_attachments);
    out.write_int32(h.status);
    out.write_uint64(static_cast<uint64_t>(h.expiration_time));
    out.write_uint64(static_cast<uint64_t>(h.issue_time));
    return true;
}

// ---- FMailMessage ------------------------------------------------------------

bool mail_message_fits(const MailMessage& m) {
    return fstring_fits(m.title) && localized_message_fits(m.localized_title) && fstring_fits(m.body) &&
           localized_message_fits(m.localized_message);
}

std::optional<MailMessage> read_mail_message(wire::PacketReader& r) {
    MailMessage m;
    auto title = read_fstring(r);
    auto localized_title = read_localized_message(r);
    auto body = read_fstring(r);
    auto localized_message = read_localized_message(r);
    auto system = read_mail_ubool(r);
    if (!title || !localized_title || !body || !localized_message || !system) return std::nullopt;
    m.title = std::move(*title);
    m.localized_title = std::move(*localized_title);
    m.body = std::move(*body);
    m.localized_message = std::move(*localized_message);
    m.system = *system;
    if (overran(r)) return std::nullopt;
    return m;
}

bool write_mail_message(emit::BunchWriter& out, const MailMessage& m) {
    if (!mail_message_fits(m)) return false;
    if (!write_fstring(out, m.title) || !write_localized_message(out, m.localized_title) || !write_fstring(out, m.body) ||
        !write_localized_message(out, m.localized_message))
        return false;
    write_mail_ubool(out, m.system);
    return true;
}

// ---- FMailItem ---------------------------------------------------------------

bool mail_item_fits(const MailItem& i) { return item_instance_fits(i.item); }

std::optional<MailItem> read_mail_item(wire::PacketReader& r) {
    auto item = read_item_instance(r);
    auto claimed = read_mail_ubool(r);
    if (!item || !claimed) return std::nullopt;
    MailItem i;
    i.item = std::move(*item);
    i.claimed = *claimed;
    if (overran(r)) return std::nullopt;
    return i;
}

bool write_mail_item(emit::BunchWriter& out, const MailItem& i) {
    if (!mail_item_fits(i)) return false;
    if (!write_item_instance(out, i.item)) return false;
    write_mail_ubool(out, i.claimed);
    return true;
}

// ---- FCurrencyContainer ------------------------------------------------------

bool currency_container_fits(const CurrencyContainer& c) {
    if (c.entries.size() > kMaxMailCurrencies) return false;
    return unique_keys(c.entries, [](const CurrencyEntry& e) { return std::make_tuple(e.tier, e.type_id); });
}

std::optional<CurrencyContainer> read_currency_container(wire::PacketReader& r) {
    const auto n = read_count(r, 192);
    if (!n || *n > kMaxMailCurrencies) return std::nullopt;
    CurrencyContainer c;
    for (uint32_t i = 0; i < *n; ++i) {
        CurrencyEntry e;
        e.tier = static_cast<int64_t>(r.read_uint64());
        e.type_id = static_cast<int64_t>(r.read_uint64());
        e.amount = static_cast<int64_t>(r.read_uint64());
        if (overran(r)) return std::nullopt;
        c.entries.push_back(e);
    }
    return c;
}
// [C-Y-B - CoA]

bool write_currency_container(emit::BunchWriter& out, const CurrencyContainer& c) {
    if (!currency_container_fits(c)) return false;
    if (!write_count(out, c.entries.size())) return false;
    for (const CurrencyEntry& e : c.entries) {
        out.write_uint64(static_cast<uint64_t>(e.tier));
        out.write_uint64(static_cast<uint64_t>(e.type_id));
        out.write_uint64(static_cast<uint64_t>(e.amount));
    }
    return true;
}

// ---- FMailPersistenceData ----------------------------------------------------

bool mail_persistence_data_fits(const MailPersistenceData& d) {
    if (!fstring_fits(d.subject) || !fstring_fits(d.sender) || !mail_message_fits(d.message)) return false;
    if (d.items.size() > kMaxMailItems) return false;
    for (const MailItem& i : d.items) {
        if (!mail_item_fits(i)) return false;
    }
    return currency_container_fits(d.currency);
}

std::optional<MailPersistenceData> read_mail_persistence_data(wire::PacketReader& r) {
    MailPersistenceData d;
    d.guid = read_guid(r);
    d.sender_guid = read_guid(r);
    d.recipient_guid = read_guid(r);
    auto subject = read_fstring(r);
    if (!subject) return std::nullopt;
    d.subject = std::move(*subject);
    d.type = r.read_uint8();
    d.batch_num = static_cast<int32_t>(r.read_uint32());
    d.created_time = static_cast<int64_t>(r.read_uint64());
    d.expiration_time = static_cast<int64_t>(r.read_uint64());
    d.node = static_cast<int64_t>(r.read_uint64());
    auto sender = read_fstring(r);
    if (!sender) return std::nullopt;
    d.sender = std::move(*sender);
    auto message = read_mail_message(r);
    if (!message) return std::nullopt;
    d.message = std::move(*message);
    const auto items = read_count(r, 33);
    if (!items || *items > kMaxMailItems) return std::nullopt;
    for (uint32_t i = 0; i < *items; ++i) {
        auto item = read_mail_item(r);
        if (!item) return std::nullopt;
        d.items.push_back(std::move(*item));
    }
    auto currency = read_currency_container(r);
    if (!currency) return std::nullopt;
    d.currency = std::move(*currency);
    auto claimed = read_mail_ubool(r);
    if (!claimed) return std::nullopt;
    d.currency_claimed = *claimed;
    d.status = static_cast<int32_t>(r.read_uint32());
    if (overran(r)) return std::nullopt;
    return d;
}
// Cyb | C O A

bool write_mail_persistence_data(emit::BunchWriter& out, const MailPersistenceData& d) {
    if (!mail_persistence_data_fits(d)) return false;
    write_guid(out, d.guid);
    write_guid(out, d.sender_guid);
    write_guid(out, d.recipient_guid);
    if (!write_fstring(out, d.subject)) return false;
    out.write_uint8(d.type);
    out.write_int32(d.batch_num);
    out.write_uint64(static_cast<uint64_t>(d.created_time));
    out.write_uint64(static_cast<uint64_t>(d.expiration_time));
    out.write_uint64(static_cast<uint64_t>(d.node));
    if (!write_fstring(out, d.sender) || !write_mail_message(out, d.message)) return false;
    if (!write_count(out, d.items.size())) return false;
    for (const MailItem& i : d.items) {
        if (!write_mail_item(out, i)) return false;
    }
    if (!write_currency_container(out, d.currency)) return false;
    write_mail_ubool(out, d.currency_claimed);
    out.write_int32(d.status);
    return true;
}

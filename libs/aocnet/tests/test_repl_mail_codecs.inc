// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_mail_codecs.inc  (included by test_repl_aoc_codecs.cpp, before its main)
//
//  The mail NetSerialize codecs (protocol/repl/mail_codec.h): FMailHeader, FMailPersistenceData
//  with FMailMessage and FMailItem, and FCurrencyContainer (lane 13, piece P2).  The expected bits
//  are built by hand in the game's wire order; the tests do not produce them with the codec under
//  test.  The three registered names go through read_struct / write_struct, the nested message and
//  item through FMailPersistenceData, and the call parameters through set_rpc_param /
//  write_rpc_params / read_rpc_params.  Every read_struct result and every get_if is checked before
//  it is used, so the old code fails these checks instead of crashing.
// ============================================================================

#include "protocol/repl/item_codec.h"
#include "protocol/repl/rpc.h"

#include <initializer_list>

// ---- helpers -----------------------------------------------------------------

/// The wire form of an ASCII FString, built by hand: the 32-bit length including the NUL (0 for an
/// empty string), the bytes and the NUL.
static void mail_put_string(BunchWriter& out, const std::string& s) {
    if (s.empty()) {
        out.write_int32(0);
        return;
    }
    out.write_int32(static_cast<int32_t>(s.size()) + 1);
    for (char c : s) out.write_uint8(static_cast<uint8_t>(c));
    out.write_uint8(0);
}

/// The wire form of a UTF-16 FString, built by hand: a negative length (units including the NUL),
/// the units and the NUL.
static void mail_put_utf16(BunchWriter& out, std::initializer_list<uint16_t> units) {
    out.write_int32(-(static_cast<int32_t>(units.size()) + 1));
    for (uint16_t u : units) out.write_uint16(u);
    out.write_uint16(0);
}

static void mail_put_guid(BunchWriter& out, const GuidValue& g) {
    out.write_uint32(g.a);
    out.write_uint32(g.b);
    out.write_uint32(g.c);
    out.write_uint32(g.d);
}

/// The wire form of an FLocalizedMessage: the table and key FStrings and the empty argument lists
/// (one 32-bit count; the codec refuses a non-zero one).
static void mail_put_localized(BunchWriter& out, const LocalizedMessage& m) {
    mail_put_string(out, m.string_table_id);
    mail_put_string(out, m.key);
    out.write_uint32(0);
}

static void mail_put_ubool(BunchWriter& out, bool b) { out.write_uint32(b ? 1u : 0u); }

static void mail_put_message(BunchWriter& out, const MailMessage& m) {
    mail_put_string(out, m.title);
    mail_put_localized(out, m.localized_title);
    mail_put_string(out, m.body);
    mail_put_localized(out, m.localized_message);
    mail_put_ubool(out, m.system);
}

/// The wire form of an FMailItem: the existing FItemInstance codec (the item is not the codec under
/// test) and the claimed flag.
static bool mail_put_item(BunchWriter& out, const MailItem& item) {
    if (!write_item_instance(out, item.item)) return false;
    mail_put_ubool(out, item.claimed);
    return true;
}

static void mail_put_currency(BunchWriter& out, const CurrencyContainer& c) {
    out.write_int32(static_cast<int32_t>(c.entries.size()));
    for (const CurrencyEntry& e : c.entries) {
        out.write_uint64(static_cast<uint64_t>(e.tier));
        out.write_uint64(static_cast<uint64_t>(e.type_id));
        out.write_uint64(static_cast<uint64_t>(e.amount));
    }
}

/// The wire form of an FMailPersistenceData, built by hand in the order the client reads it.
static bool mail_put_persistence(BunchWriter& out, const MailPersistenceData& d) {
    mail_put_guid(out, d.guid);
    mail_put_guid(out, d.sender_guid);
    mail_put_guid(out, d.recipient_guid);
    mail_put_string(out, d.subject);
    out.write_bits(d.type, 8);
    out.write_int32(d.batch_num);
    out.write_uint64(static_cast<uint64_t>(d.created_time));
    out.write_uint64(static_cast<uint64_t>(d.expiration_time));
    out.write_uint64(static_cast<uint64_t>(d.node));
    mail_put_string(out, d.sender);
    mail_put_message(out, d.message);
    out.write_int32(static_cast<int32_t>(d.items.size()));
    for (const MailItem& item : d.items) {
        if (!mail_put_item(out, item)) return false;
    }
    mail_put_currency(out, d.currency);
    mail_put_ubool(out, d.currency_claimed);
    out.write_int32(d.status);
    return true;
}

// ---- samples (obviously fake) ------------------------------------------------

static MailHeader mail_sample_header() {
    MailHeader h;
    h.guid = guid(0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u);
    h.sender = "Fake Sender";
    h.subject = "Fake Subject";
    h.localized_subject = LocalizedMessage{"FakeTable", "FakeSubjectKey"};
    h.has_attachments = true;
    h.status = 6;
    h.expiration_time = 0x0102030405060708ll;
    h.issue_time = 1763000000123ll;
    return h;
}

static ItemInstance mail_plain_item(uint64_t record, uint32_t quantity, GuidValue g) {
    ItemInstance i;
    i.has_item = true;
    i.record_id = record;
    i.quantity = quantity;
    i.guid = g;
    return i;
}

static MailPersistenceData mail_sample_persistence() {
    MailPersistenceData d;
    d.guid = guid(0x10101010u, 0x20202020u, 0x30303030u, 0x40404040u);
    d.sender_guid = guid(0x51515151u, 0x62626262u, 0x73737373u, 0x84848484u);
    d.recipient_guid = guid(0x95959595u, 0xa6a6a6a6u, 0xb7b7b7b7u, 0xc8c8c8c8u);
    d.subject = "Fake subject";
    d.type = 3;
    d.batch_num = -7;
    d.created_time = 1763000000000ll;
    d.expiration_time = 1764000000000ll;
    d.node = 0x1122334455667788ll;
    d.sender = "Fake Sender";
    d.message.title = "Fake title";
    d.message.localized_title = LocalizedMessage{"FakeTable", "FakeTitleKey"};
    d.message.body = "Fake body";
    d.message.localized_message = LocalizedMessage{};
    d.message.system = false;
    d.items = {MailItem{mail_plain_item(0x1111222233334444ull, 3, guid(0xa1a1a1a1u, 0xa2a2a2a2u, 0xa3a3a3a3u, 0xa4a4a4a4u)), false}};
    d.currency.entries = {CurrencyEntry{0x1111222233334444ll, 0x5555666677778888ll, 0x4059000000000000ll}};
    d.currency_claimed = true;
    d.status = 6;
    return d;
}

// ---- FMailHeader -------------------------------------------------------------

static void test_mail_header() {
    std::printf("FMailHeader\n");
    CHECK(is_known_struct("FMailHeader"), "FMailHeader is a registered NetSerialize struct");
    const MailHeader h = mail_sample_header();
    BunchWriter want;
    mail_put_guid(want, h.guid);
    mail_put_string(want, h.sender);
    mail_put_string(want, h.subject);
    mail_put_localized(want, h.localized_subject);
    mail_put_ubool(want, h.has_attachments);
    want.write_int32(h.status);
    want.write_uint64(static_cast<uint64_t>(h.expiration_time));
    want.write_uint64(static_cast<uint64_t>(h.issue_time));
    const size_t expect = 128 + (32 + 8 * (h.sender.size() + 1)) + (32 + 8 * (h.subject.size() + 1)) +
                          (32 + 8 * (h.localized_subject.string_table_id.size() + 1)) +
                          (32 + 8 * (h.localized_subject.key.size() + 1)) + 32 + 32 + 32 + 64 + 64;
    CHECK(want.bit_pos() == expect,
          "the hand-built header is the guid, the sender, the subject, the localized subject, the attachment flag, the status, the expiration time and the issue time");
    PacketReader r(want.data(), want.byte_size(), want.bit_pos());
    const auto got = read_struct("FMailHeader", r);
    CHECK(got.has_value(), "FMailHeader reads from the hand-built bits");
    CHECK(got && r.pos() == want.bit_pos(), "the read consumes every bit");
    const MailHeader* back = got ? got->get_if<MailHeader>() : nullptr;
    CHECK(back != nullptr, "the value is a MailHeader");
    CHECK(back && *back == h,
          "the sender, the subject, the localized subject, the attachment flag, the status, the expiration time and the issue time come back");
    CHECK(back && back->status == h.status && back->expiration_time == h.expiration_time && back->issue_time == h.issue_time && back->has_attachments,
          "the status is the 32 bits after the attachment flag, and the expiration time and the issue time are the last 128");
    BunchWriter out;
    const bool wrote = write_struct("FMailHeader", Value(h), out);
    CHECK(wrote, "FMailHeader writes");
    CHECK(wrote && same_as(out, want.bytes(), 0, want.bit_pos()), "the writer writes the exact hand-built bits");
    BunchWriter bad;
    CHECK(!struct_accepts("FMailHeader", Value(Vec3d{})) && !write_struct("FMailHeader", Value(Vec3d{}), bad) && bad.bit_pos() == 0,
          "another value type is not written");
    MailHeader nul = h;
    nul.sender = std::string("a\0b", 3);
    MailHeader bad_utf8 = h;
    bad_utf8.subject = "\xff";
    BunchWriter bad2;
    CHECK(!struct_accepts("FMailHeader", Value(nul)) && !struct_accepts("FMailHeader", Value(bad_utf8)) &&
              !write_struct("FMailHeader", Value(nul), bad2) && bad2.bit_pos() == 0,
          "a NUL or a byte that is not UTF-8 in a string is not written");
}

// ---- the header's expiration time --------------------------------------------

static void test_mail_header_expiry_field() {
    std::printf("the header's expiration time\n");
    MailHeader h = mail_sample_header();
    h.expiration_time = 0x0f1e2d3c4b5a6978ll;   // obviously fake
    BunchWriter out;
    const bool wrote = write_struct("FMailHeader", Value(h), out);
    CHECK(wrote, "a header with an expiration time writes");
    BunchWriter want;
    mail_put_guid(want, h.guid);
    mail_put_string(want, h.sender);
    mail_put_string(want, h.subject);
    mail_put_localized(want, h.localized_subject);
    mail_put_ubool(want, h.has_attachments);
    want.write_int32(h.status);
    const size_t expiry_bit = want.bit_pos();
    want.write_uint64(static_cast<uint64_t>(h.expiration_time));
    want.write_uint64(static_cast<uint64_t>(h.issue_time));
    CHECK(wrote && same_as(out, want.bytes(), 0, want.bit_pos()), "the writer writes the whole hand-built header");
    const std::vector<uint8_t> got_expiry = slice_bits(out.bytes(), expiry_bit, 64);
    const std::vector<uint8_t> want_expiry = slice_bits(want.bytes(), expiry_bit, 64);
    CHECK(wrote && got_expiry == want_expiry, "the writer writes the expiration time in the 64 bits after the status");
    PacketReader r(out.data(), out.byte_size(), out.bit_pos());
    const auto got = read_struct("FMailHeader", r);
    CHECK(got.has_value(), "the written header reads back");
    const MailHeader* back = got ? got->get_if<MailHeader>() : nullptr;
    CHECK(back != nullptr, "the value is a MailHeader");
    CHECK(back && r.pos() == out.bit_pos() && back->expiration_time == h.expiration_time,
          "the read puts the expiration time back into expiration_time");
}

// ---- FMailMessage inside FMailPersistenceData --------------------------------

static void test_mail_message() {
    std::printf("FMailMessage inside FMailPersistenceData\n");
    CHECK(is_known_struct("FMailPersistenceData"), "FMailPersistenceData is a registered NetSerialize struct");
    MailPersistenceData d = mail_sample_persistence();
    d.message.title = "Fake title";
    d.message.localized_title = LocalizedMessage{"FakeTable", "FakeTitleKey"};
    d.message.body = "Fake body";
    d.message.localized_message = LocalizedMessage{};
    d.message.system = true;
    BunchWriter want;
    const bool built = mail_put_persistence(want, d);
    CHECK(built, "the test builds the hand-made persistence bits");
    PacketReader r(want.data(), want.byte_size(), want.bit_pos());
    const auto got = read_struct("FMailPersistenceData", r);
    CHECK(got.has_value(), "a persistence with all five message fields reads");
    CHECK(got && r.pos() == want.bit_pos(), "the read consumes every bit");
    const MailPersistenceData* p = got ? got->get_if<MailPersistenceData>() : nullptr;
    CHECK(p != nullptr, "the value is a MailPersistenceData");
    CHECK(p && p->message == d.message && p->subject == d.subject && p->sender == d.sender,
          "the title, the localized title with its table and key, the body, the localized message and the system flag come back");
    BunchWriter out;
    const bool wrote = write_struct("FMailPersistenceData", Value(d), out);
    CHECK(wrote, "the persistence writes");
    CHECK(wrote && same_as(out, want.bytes(), 0, want.bit_pos()), "the message's bits are the hand-built ones");
    BunchWriter bad;
    CHECK(!struct_accepts("FMailPersistenceData", Value(Vec3d{})) &&
              !write_struct("FMailPersistenceData", Value(Vec3d{}), bad) && bad.bit_pos() == 0,
          "a wrong value kind is not written");
}

// ---- FMailItem inside FMailPersistenceData -----------------------------------

static void test_mail_item() {
    std::printf("FMailItem inside FMailPersistenceData\n");
    CHECK(is_known_struct("FMailPersistenceData"), "FMailPersistenceData is a registered NetSerialize struct");
    const MailItem empty{MailItem{ItemInstance{}, false}};
    const MailItem plain{MailItem{mail_plain_item(0x1111222233334444ull, 3, guid(0xa1a1a1a1u, 0xa2a2a2a2u, 0xa3a3a3a3u, 0xa4a4a4a4u)), true}};
    const MailItem items[2] = {empty, plain};
    for (int k = 0; k < 2; ++k) {
        MailPersistenceData d;   // the zero mail: the item is the only thing that varies here
        d.items = {items[k]};
        BunchWriter want;
        const bool built = mail_put_persistence(want, d);
        CHECK(built, "the test builds the hand-made item bits");
        PacketReader r(want.data(), want.byte_size(), want.bit_pos());
        const auto got = read_struct("FMailPersistenceData", r);
        CHECK(got.has_value(), "the persistence with the item reads");
        CHECK(got && r.pos() == want.bit_pos(), "the read consumes every bit");
        const MailPersistenceData* p = got ? got->get_if<MailPersistenceData>() : nullptr;
        CHECK(p != nullptr, "the value is a MailPersistenceData");
        CHECK(p && p->items.size() == 1 && p->items[0] == items[k],
              k == 0 ? "the empty item (one 0 bit) and a false claimed flag come back"
                     : "the nested item is exactly the existing FItemInstance and the claimed flag is true");
        BunchWriter out;
        const bool wrote = write_struct("FMailPersistenceData", Value(d), out);
        CHECK(wrote, "the persistence writes");
        CHECK(wrote && same_as(out, want.bytes(), 0, want.bit_pos()), "the item's bits are the hand-built ones");
        const auto t = trip("FMailPersistenceData", Value(d));
        CHECK(t.whole && t.bits == (k == 0 ? 1096 + 1 + 32 : 1096 + 1096 + 32),
              k == 0 ? "an empty item is one 0 bit and the claimed UBool: the persistence is 1,129 bits"
                     : "a plain item is 1,096 bits and the claimed UBool: the persistence is 2,224 bits");
    }
}

// ---- a mail with items only --------------------------------------------------

static void test_mail_items_only() {
    std::printf("a mail with items only (an empty currency container)\n");
    CHECK(is_known_struct("FMailPersistenceData"), "FMailPersistenceData is a registered NetSerialize struct");
    const ItemInstance item = mail_plain_item(0x1111222233334444ull, 3, guid(0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u));
    MailPersistenceData d = mail_sample_persistence();
    d.items = {MailItem{item, false}};
    d.currency = CurrencyContainer{};
    d.currency_claimed = false;
    BunchWriter want;
    const bool built = mail_put_persistence(want, d);
    CHECK(built, "the test builds the hand-made bits");
    PacketReader r(want.data(), want.byte_size(), want.bit_pos());
    const auto got = read_struct("FMailPersistenceData", r);
    CHECK(got.has_value(), "a mail with one item and no currency reads");
    CHECK(got && r.pos() == want.bit_pos(), "the read consumes every bit");
    const MailPersistenceData* p = got ? got->get_if<MailPersistenceData>() : nullptr;
    CHECK(p != nullptr, "the value is a MailPersistenceData");
    CHECK(p && p->currency.entries.empty() && p->items.size() == 1, "the currency count is 0 and one item comes back");
    CHECK(p && p->items[0].item.record_id == item.record_id && p->items[0].item.quantity == item.quantity &&
              p->items[0].item.guid == item.guid && !p->items[0].claimed,
          "the item's own record id, quantity and guid come back, unclaimed");
    const auto t = trip("FMailPersistenceData", Value(d));
    CHECK(t.whole && t.value && t.value->get_if<MailPersistenceData>() && *t.value->get_if<MailPersistenceData>() == d,
          "the mail round trips through the writer and the reader");
}

// ---- FCurrencyContainer ------------------------------------------------------

static void test_mail_currency() {
    std::printf("FCurrencyContainer\n");
    CHECK(is_known_struct("FCurrencyContainer"), "FCurrencyContainer is a registered NetSerialize struct");
    BunchWriter empty_w;
    empty_w.write_int32(0);
    PacketReader er(empty_w.data(), empty_w.byte_size(), empty_w.bit_pos());
    const auto empty = read_struct("FCurrencyContainer", er);
    CHECK(empty.has_value(), "an empty container reads");
    const CurrencyContainer* ec = empty ? empty->get_if<CurrencyContainer>() : nullptr;
    CHECK(ec != nullptr, "the value is a CurrencyContainer");
    CHECK(ec && ec->entries.empty(), "with no entries");
    BunchWriter empty_out;
    CHECK(write_struct("FCurrencyContainer", Value(CurrencyContainer{}), empty_out) && empty_out.bit_pos() == 32,
          "an empty container is its count alone: 32 bits");

    CurrencyContainer c;
    c.entries.push_back(CurrencyEntry{0x1111222233334444ll, 0x5555666677778888ll, 0x4059000000000000ll});
    BunchWriter want;
    mail_put_currency(want, c);
    CHECK(want.bit_pos() == 224, "one entry is 224 bits: the count, two int64 and 8 bytes");
    PacketReader r(want.data(), want.byte_size(), want.bit_pos());
    const auto got = read_struct("FCurrencyContainer", r);
    CHECK(got.has_value(), "one entry reads");
    CHECK(got && r.pos() == want.bit_pos(), "the read consumes every bit");
    const CurrencyContainer* back = got ? got->get_if<CurrencyContainer>() : nullptr;
    CHECK(back != nullptr, "the value is a CurrencyContainer");
    CHECK(back && *back == c, "the currency, its type id and its 8-byte amount come back");
    CHECK(back && back->entries.size() == 1 && back->entries[0].tier == c.entries[0].tier &&
              back->entries[0].type_id == c.entries[0].type_id && back->entries[0].amount == c.entries[0].amount,
          "the amount's 8 bytes are the exact bits");
    BunchWriter out;
    const bool wrote = write_struct("FCurrencyContainer", Value(c), out);
    CHECK(wrote, "the container writes");
    CHECK(wrote && same_as(out, want.bytes(), 0, want.bit_pos()), "the writer writes the exact hand-built bits");

    // One item and one currency entry: the widest case of the mail's own bits.
    const ItemInstance item = mail_plain_item(0x1111222233334444ull, 3, guid(0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u));
    MailPersistenceData d = mail_sample_persistence();
    d.items = {MailItem{item, false}};
    d.currency = c;
    d.currency_claimed = true;
    BunchWriter want_mail;
    const bool built = mail_put_persistence(want_mail, d);
    CHECK(built, "the test builds the hand-made mail bits");
    PacketReader mr(want_mail.data(), want_mail.byte_size(), want_mail.bit_pos());
    const auto mail = read_struct("FMailPersistenceData", mr);
    CHECK(mail.has_value(), "a mail with one item and one currency entry reads");
    CHECK(mail && mr.pos() == want_mail.bit_pos(), "the read consumes every bit");
    const MailPersistenceData* p = mail ? mail->get_if<MailPersistenceData>() : nullptr;
    CHECK(p != nullptr, "the value is a MailPersistenceData");
    CHECK(p && p->items.size() == 1 && p->items[0].item == item && p->currency == c && p->currency_claimed,
          "the item, the entry's currency, type id and amount and the claimed flag come back");

    CurrencyContainer dup = c;
    dup.entries.push_back(CurrencyEntry{c.entries[0].tier, c.entries[0].type_id, 1});
    BunchWriter bad;
    CHECK(!struct_accepts("FCurrencyContainer", Value(dup)) && !write_struct("FCurrencyContainer", Value(dup), bad) &&
              bad.bit_pos() == 0,
          "a repeated (currency, type id) key is not written");
    CurrencyContainer over;
    for (int i = 0; i < 257; ++i) over.entries.push_back(CurrencyEntry{i + 1, 1000 + i, 0});
    CHECK(!struct_accepts("FCurrencyContainer", Value(over)) && !write_struct("FCurrencyContainer", Value(over), bad),
          "257 currency entries are not written");
}

// ---- a mail with several items -----------------------------------------------

static void test_mail_several_items() {
    std::printf("a mail with several items\n");
    CHECK(is_known_struct("FMailPersistenceData"), "FMailPersistenceData is a registered NetSerialize struct");
    MailPersistenceData d = mail_sample_persistence();
    d.items.clear();
    for (int i = 0; i < 3; ++i) {
        const ItemInstance item = mail_plain_item(0x1000 + i, static_cast<uint32_t>(i + 1),
                                                  guid(static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i + 2),
                                                       static_cast<uint32_t>(i + 3), static_cast<uint32_t>(i + 4)));
        d.items.push_back(MailItem{item, i == 1});
    }
    d.currency = CurrencyContainer{};
    BunchWriter want;
    const bool built = mail_put_persistence(want, d);
    CHECK(built, "the test builds the hand-made bits");
    PacketReader r(want.data(), want.byte_size(), want.bit_pos());
    const auto got = read_struct("FMailPersistenceData", r);
    CHECK(got.has_value(), "three items read");
    CHECK(got && r.pos() == want.bit_pos(), "the read consumes every bit");
    const MailPersistenceData* p = got ? got->get_if<MailPersistenceData>() : nullptr;
    CHECK(p != nullptr, "the value is a MailPersistenceData");
    CHECK(p && p->items.size() == 3, "the count is 3");
    bool each = p && p->items.size() == 3;
    if (p && p->items.size() == 3) {
        for (int i = 0; i < 3; ++i) {
            each = each && p->items[i].item.record_id == 0x1000 + i && p->items[i].item.quantity == static_cast<uint32_t>(i + 1) &&
                   p->items[i].item.guid == d.items[i].item.guid && p->items[i].claimed == (i == 1);
        }
    }
    CHECK(each, "each item is compared by its own record id, quantity, guid and claimed flag");
    const auto t = trip("FMailPersistenceData", Value(d));
    CHECK(t.whole && t.value && t.value->get_if<MailPersistenceData>() && *t.value->get_if<MailPersistenceData>() == d,
          "the three items round trip");
}

// ---- strings -----------------------------------------------------------------

static void test_mail_strings() {
    std::printf("FMailHeader strings\n");
    CHECK(is_known_struct("FMailHeader"), "FMailHeader is a registered NetSerialize struct");
    const MailHeader empty;
    BunchWriter empty_want;
    mail_put_guid(empty_want, empty.guid);
    mail_put_string(empty_want, "");
    mail_put_string(empty_want, "");
    mail_put_localized(empty_want, empty.localized_subject);
    mail_put_ubool(empty_want, empty.has_attachments);
    empty_want.write_int32(empty.status);
    empty_want.write_uint64(static_cast<uint64_t>(empty.expiration_time));
    empty_want.write_uint64(static_cast<uint64_t>(empty.issue_time));
    CHECK(empty_want.bit_pos() == 480, "empty strings and an empty localized message: the header is 480 bits");
    const auto e = trip("FMailHeader", Value(empty));
    CHECK(e.whole && e.bits == 480 && e.value && e.value->get_if<MailHeader>() && *e.value->get_if<MailHeader>() == empty,
          "an empty header round trips; the two localized-message counts are zero");

    MailHeader h;
    h.sender = "S\xc3\xa4nder";   // the UTF-16 units below
    h.subject = std::string(1024, 's');
    BunchWriter want;
    mail_put_guid(want, h.guid);
    mail_put_utf16(want, {'S', 0x00e4, 'n', 'd', 'e', 'r'});
    mail_put_string(want, h.subject);
    mail_put_localized(want, h.localized_subject);
    mail_put_ubool(want, h.has_attachments);
    want.write_int32(h.status);
    want.write_uint64(static_cast<uint64_t>(h.expiration_time));
    want.write_uint64(static_cast<uint64_t>(h.issue_time));
    const auto t = trip("FMailHeader", Value(h));
    CHECK(t.whole && t.bits == 128 + (32 + 16 * 7) + (32 + 8 * 1025) + 96 + 32 + 32 + 64 + 64,
          "a 1,024-character ASCII subject and a non-ASCII sender: 8,792 bits");
    CHECK(t.value && t.value->get_if<MailHeader>() && *t.value->get_if<MailHeader>() == h,
          "the 1,024-character subject and the UTF-16 sender come back");

    MailHeader longest;
    longest.subject = std::string(65535, 'x');
    const auto l = trip("FMailHeader", Value(longest));
    CHECK(l.whole && l.bits == 128 + 32 + (32 + 8 * 65536) + 96 + 32 + 32 + 64 + 64,
          "a 65,535-character subject, the longest the codec accepts: 524,704 bits");
    CHECK(l.value && l.value->get_if<MailHeader>() && *l.value->get_if<MailHeader>() == longest,
          "the longest subject comes back");
    MailHeader too_long;
    too_long.subject = std::string(65536, 'x');
    BunchWriter bad;
    CHECK(!struct_accepts("FMailHeader", Value(too_long)) && !write_struct("FMailHeader", Value(too_long), bad) && bad.bit_pos() == 0,
          "a 65,536-character string is not written");
}

// ---- localized message arguments ---------------------------------------------

static void test_localized_message_refused() {
    std::printf("FLocalizedMessage argument lists\n");
    CHECK(is_known_struct("FMailHeader"), "FMailHeader is a registered NetSerialize struct");
    const MailHeader h;
    BunchWriter good;
    mail_put_guid(good, h.guid);
    mail_put_string(good, h.sender);
    mail_put_string(good, h.subject);
    mail_put_localized(good, h.localized_subject);
    mail_put_ubool(good, h.has_attachments);
    good.write_int32(h.status);
    good.write_uint64(static_cast<uint64_t>(h.expiration_time));
    good.write_uint64(static_cast<uint64_t>(h.issue_time));
    const auto read = read_bits_as("FMailHeader", good);
    CHECK(read.has_value(), "a header with an empty localized message reads");

    BunchWriter format_one;
    mail_put_guid(format_one, h.guid);
    mail_put_string(format_one, h.sender);
    mail_put_string(format_one, h.subject);
    mail_put_string(format_one, "");
    mail_put_string(format_one, "");
    format_one.write_uint16(1);   // FormatArgs count 1
    format_one.write_uint16(0);   // LookupArgs count 0
    CHECK(!read_bits_as("FMailHeader", format_one).has_value(), "a format-argument count of 1 is refused on the read side");

    BunchWriter lookup_one;
    mail_put_guid(lookup_one, h.guid);
    mail_put_string(lookup_one, h.sender);
    mail_put_string(lookup_one, h.subject);
    mail_put_string(lookup_one, "");
    mail_put_string(lookup_one, "");
    lookup_one.write_uint16(0);
    lookup_one.write_uint16(1);   // LookupArgs count 1
    CHECK(!read_bits_as("FMailHeader", lookup_one).has_value(), "a lookup-argument count of 1 is refused on the read side");

    BunchWriter out;
    const bool wrote = write_struct("FMailHeader", Value(h), out);
    CHECK(wrote, "the writer writes the empty localized message");
    CHECK(wrote && out.bit_pos() == 480, "it writes both zero argument counts: 480 bits");
}

// ---- the registry and refusals -----------------------------------------------

static void test_mail_registry_and_refusals() {
    std::printf("the mail registry and malformed input\n");
    CHECK(is_known_struct("FMailHeader") && is_known_struct("FMailPersistenceData") && is_known_struct("FCurrencyContainer"),
          "the three mail structs are registered by name");
    CHECK(!is_known_struct("FMailMessage") && !is_known_struct("FMailItem"),
          "FMailMessage and FMailItem are plain members, not registered names");
    const MailHeader h = mail_sample_header();
    const MailPersistenceData d = mail_sample_persistence();
    CHECK(struct_accepts("FMailHeader", Value(h)) && struct_accepts("FMailPersistenceData", Value(d)) &&
              struct_accepts("FCurrencyContainer", Value(CurrencyContainer{})),
          "struct_accepts takes each struct's own value type");
    CHECK(!struct_accepts("FMailHeader", Value(MailPersistenceData{})) && !struct_accepts("FMailPersistenceData", Value(h)) &&
              !struct_accepts("FCurrencyContainer", Value(h)),
          "it refuses the other mail values");
    BunchWriter bad;
    CHECK(!write_struct("FMailHeader", Value(Vec3d{}), bad) && bad.bit_pos() == 0, "a wrong value kind writes nothing");

    BunchWriter want;
    mail_put_guid(want, h.guid);
    mail_put_string(want, h.sender);
    mail_put_string(want, h.subject);
    mail_put_localized(want, h.localized_subject);
    want.write_uint32(2);   // a UBool above 1
    want.write_int32(h.status);
    want.write_uint64(static_cast<uint64_t>(h.expiration_time));
    want.write_uint64(static_cast<uint64_t>(h.issue_time));
    CHECK(!read_bits_as("FMailHeader", want).has_value(), "a 32-bit bool above 1 is refused");

    BunchWriter negative;
    negative.write_int32(-1);
    CHECK(!read_bits_as("FCurrencyContainer", negative).has_value(), "a negative count is refused");
    BunchWriter cut;
    cut.write_int32(1);
    cut.write_uint64(1);
    cut.write_uint64(2);
    cut.write_uint64(0);
    PacketReader short_reader(cut.data(), cut.byte_size(), cut.bit_pos() - 1);
    CHECK(!read_struct("FCurrencyContainer", short_reader).has_value(), "a cut value is refused");

    PacketReader first(want.data(), want.byte_size(), 4 + 4 + 8);   // only part of it: both reads fail the same way
    PacketReader second(want.data(), want.byte_size(), 4 + 4 + 8);
    const auto a = read_struct("FMailHeader", first);
    const auto b = read_struct("FMailHeader", second);
    CHECK(!a.has_value() && !b.has_value(), "the same cut bits read twice give the same refusal");
    BunchWriter full;
    mail_put_guid(full, h.guid);
    mail_put_string(full, h.sender);
    mail_put_string(full, h.subject);
    mail_put_localized(full, h.localized_subject);
    mail_put_ubool(full, h.has_attachments);
    full.write_int32(h.status);
    full.write_uint64(static_cast<uint64_t>(h.expiration_time));
    full.write_uint64(static_cast<uint64_t>(h.issue_time));
    PacketReader r1(full.data(), full.byte_size(), full.bit_pos());
    PacketReader r2(full.data(), full.byte_size(), full.bit_pos());
    const auto x = read_struct("FMailHeader", r1);
    const auto y = read_struct("FMailHeader", r2);
    CHECK(x && y && *x == *y, "the same bits read twice give the same value");
}

// ---- an item that does not fit -------------------------------------------------

static void test_mail_item_bad() {
    std::printf("an item inside a mail that does not fit\n");
    CHECK(is_known_struct("FMailPersistenceData"), "FMailPersistenceData is a registered NetSerialize struct");
    MailPersistenceData good = mail_sample_persistence();
    good.items = {MailItem{mail_plain_item(0x1111222233334444ull, 3, guid(1, 2, 3, 4)), false}};
    BunchWriter out;
    const bool wrote = write_struct("FMailPersistenceData", Value(good), out);
    CHECK(wrote, "a valid item inside the mail writes");

    ItemInstance odd;                 // the empty item with a field set
    odd.quantity = 3;
    MailPersistenceData bad = mail_sample_persistence();
    bad.items = {MailItem{odd, false}};
    BunchWriter refused;
    CHECK(!struct_accepts("FMailPersistenceData", Value(bad)) && !write_struct("FMailPersistenceData", Value(bad), refused) &&
              refused.bit_pos() == 0,
          "an empty item with a field set makes the mail unwritable");

    ItemInstance nul = mail_plain_item(0x1111222233334444ull, 3, guid(1, 2, 3, 4));
    nul.custom_data.push_back(PersistedCustomData{0, std::string("a\0b", 3)});
    bad.items = {MailItem{nul, false}};
    CHECK(!struct_accepts("FMailPersistenceData", Value(bad)) && !write_struct("FMailPersistenceData", Value(bad), refused),
          "an item whose custom text has a NUL makes the mail unwritable");

    // The read side: the claimed UBool must be 0 or 1, and a cut item is refused.
    MailPersistenceData claimed_bad = good;
    BunchWriter claimed_want;
    mail_put_persistence(claimed_want, claimed_bad);
    PacketReader claimed_reader(claimed_want.data(), claimed_want.byte_size(), claimed_want.bit_pos());
    CHECK(read_struct("FMailPersistenceData", claimed_reader).has_value(),
          "a valid item's claimed flag reads");   // the positive read, red on the old code
    BunchWriter two;
    mail_put_guid(two, claimed_bad.guid);
    mail_put_guid(two, claimed_bad.sender_guid);
    mail_put_guid(two, claimed_bad.recipient_guid);
    mail_put_string(two, claimed_bad.subject);
    two.write_bits(claimed_bad.type, 8);
    two.write_int32(claimed_bad.batch_num);
    two.write_uint64(static_cast<uint64_t>(claimed_bad.created_time));
    two.write_uint64(static_cast<uint64_t>(claimed_bad.expiration_time));
    two.write_uint64(static_cast<uint64_t>(claimed_bad.node));
    mail_put_string(two, claimed_bad.sender);
    mail_put_message(two, claimed_bad.message);
    two.write_int32(1);
    write_item_instance(two, claimed_bad.items[0].item);
    two.write_uint32(2);   // claimed: a UBool above 1
    mail_put_currency(two, claimed_bad.currency);
    mail_put_ubool(two, claimed_bad.currency_claimed);
    two.write_int32(claimed_bad.status);
    CHECK(!read_bits_as("FMailPersistenceData", two).has_value(), "a claimed flag of 2 inside the item is refused");

    BunchWriter cut;
    mail_put_guid(cut, claimed_bad.guid);
    mail_put_guid(cut, claimed_bad.sender_guid);
    mail_put_guid(cut, claimed_bad.recipient_guid);
    mail_put_string(cut, claimed_bad.subject);
    cut.write_bits(claimed_bad.type, 8);
    cut.write_int32(claimed_bad.batch_num);
    cut.write_uint64(static_cast<uint64_t>(claimed_bad.created_time));
    cut.write_uint64(static_cast<uint64_t>(claimed_bad.expiration_time));
    cut.write_uint64(static_cast<uint64_t>(claimed_bad.node));
    mail_put_string(cut, claimed_bad.sender);
    mail_put_message(cut, claimed_bad.message);
    cut.write_int32(1);
    cut.write_bit(0);   // the start of the empty item, its claimed flag missing
    CHECK(!read_bits_as("FMailPersistenceData", cut).has_value(), "a cut item inside the mail is refused");
}

// ---- the bounds -----------------------------------------------------------------

static void test_mail_bounds() {
    std::printf("the item and currency bounds\n");
    CHECK(is_known_struct("FMailPersistenceData"), "FMailPersistenceData is a registered NetSerialize struct");
    MailPersistenceData d = mail_sample_persistence();
    d.items.assign(1024, MailItem{});
    d.currency.entries.clear();
    for (size_t i = 0; i < 256; ++i) {
        d.currency.entries.push_back(CurrencyEntry{static_cast<int64_t>(i + 1), static_cast<int64_t>(1000 + i), 0});
    }
    BunchWriter out;
    const bool wrote = write_struct("FMailPersistenceData", Value(d), out);
    CHECK(wrote, "1,024 items and 256 currency entries write");
    BunchWriter want;
    const bool built = mail_put_persistence(want, d);
    CHECK(built, "the test builds the hand-made bits at the limit");
    CHECK(wrote && built && same_as(out, want.bytes(), 0, want.bit_pos()), "the writer writes the exact hand-built bits at the limit");
    PacketReader r(want.data(), want.byte_size(), want.bit_pos());
    const auto got = read_struct("FMailPersistenceData", r);
    CHECK(got.has_value(), "1,024 items and 256 currency entries read");
    CHECK(got && r.pos() == want.bit_pos(), "the read consumes every bit");
    const MailPersistenceData* p = got ? got->get_if<MailPersistenceData>() : nullptr;
    CHECK(p != nullptr, "the value is a MailPersistenceData");
    CHECK(p && p->items.size() == 1024 && p->currency.entries.size() == 256,
          "the counts are exactly 1,024 items and 256 entries");

    MailPersistenceData over_items = d;
    over_items.items.push_back(MailItem{});
    MailPersistenceData over_currency = d;
    over_currency.currency.entries.push_back(CurrencyEntry{257, 2000, 0});
    BunchWriter refused;
    CHECK(!struct_accepts("FMailPersistenceData", Value(over_items)) && !write_struct("FMailPersistenceData", Value(over_items), refused) &&
              refused.bit_pos() == 0,
          "1,025 items are not written");
    CHECK(!struct_accepts("FMailPersistenceData", Value(over_currency)) && !write_struct("FMailPersistenceData", Value(over_currency), refused),
          "257 currency entries are not written");
}

// ---- the status bits ------------------------------------------------------------

static void test_mail_status() {
    std::printf("the status bits\n");
    CHECK(is_known_struct("FMailHeader"), "FMailHeader is a registered NetSerialize struct");
    MailHeader h = mail_sample_header();
    h.status = 6;
    const auto ht = trip("FMailHeader", Value(h));
    const MailHeader* hb = ht.value ? ht.value->get_if<MailHeader>() : nullptr;
    CHECK(ht.whole && hb && *hb == h && (hb->status & 2) != 0 && (hb->status & 4) != 0,
          "a header with the read (4) and deleted (2) bits together round trips");
    MailPersistenceData d = mail_sample_persistence();
    d.status = 6;
    const auto dt = trip("FMailPersistenceData", Value(d));
    const MailPersistenceData* db = dt.value ? dt.value->get_if<MailPersistenceData>() : nullptr;
    CHECK(dt.whole && db && *db == d && (db->status & 2) != 0 && (db->status & 4) != 0,
          "a persistence with the read (4) and deleted (2) bits together round trips");
}

// ---- a call's parameter of each struct ------------------------------------------

static void test_mail_call_params() {
    std::printf("a call's parameter of each registered struct\n");
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    const schema::ClassSchema& social = schema_named("UIntrepidSocialComponent");
    const MailHeader header = mail_sample_header();
    {   // ClientUpdateMailHeaders: Headers[] of FMailHeader, Version, bComplete
        const schema::RpcSchema* rpc = pc.rpc("ClientUpdateMailHeaders");
        CHECK(rpc != nullptr, "ClientUpdateMailHeaders is a PlayerController call");
        RpcParams p;
        ArrayValue headers;
        ElementState e;
        e.values[1] = Value(header);
        headers.elements.push_back(e);
        CHECK(set_rpc_param(*rpc, p, "Headers", Value(headers)), "Headers takes an FMailHeader element");
        CHECK(set_rpc_param(*rpc, p, "Version", Value::u64(7)) && set_rpc_param(*rpc, p, "bComplete", Value::boolean(true)),
              "Version and bComplete are set");
        BunchWriter out;
        bool failed = false;
        const size_t bits = write_rpc_params(*rpc, p, out, {}, &failed);
        CHECK(!failed && bits > 0, "the call writes");
        size_t pos = 0;
        const auto back = read_rpc_params(*rpc, out.data(), out.bit_pos(), pos);
        CHECK(back.has_value() && pos == out.bit_pos(), "the call reads back to its last bit");
        const ArrayValue* ba = back && back->values.count(1) ? back->values.at(1).get_if<ArrayValue>() : nullptr;
        CHECK(ba && ba->elements.size() == 1 && ba->elements[0].values.count(1) &&
                  ba->elements[0].values.at(1) == Value(header),
              "the header comes back equal");
        const uint64_t* version = back && back->values.count(2) ? back->values.at(2).get_if<uint64_t>() : nullptr;
        const bool* complete = back && back->values.count(3) ? back->values.at(3).get_if<bool>() : nullptr;
        CHECK(version && *version == 7 && complete && *complete, "Version and bComplete come back with it");
    }
    {   // ClientGetMailData: MailData = FMailPersistenceData
        const schema::RpcSchema* rpc = social.rpc("ClientGetMailData");
        CHECK(rpc != nullptr, "ClientGetMailData is a social-component call");
        MailPersistenceData data = mail_sample_persistence();
        RpcParams p;
        CHECK(set_rpc_param(*rpc, p, "MailData", Value(data)), "MailData takes an FMailPersistenceData");
        BunchWriter out;
        bool failed = false;
        write_rpc_params(*rpc, p, out, {}, &failed);
        CHECK(!failed && out.bit_pos() > 0, "the call writes");
        size_t pos = 0;
        const auto back = read_rpc_params(*rpc, out.data(), out.bit_pos(), pos);
        CHECK(back.has_value() && pos == out.bit_pos(), "the call reads back to its last bit");
        const MailPersistenceData* data_back = back && back->values.count(1) ? back->values.at(1).get_if<MailPersistenceData>() : nullptr;
        CHECK(data_back && *data_back == data, "the persistence data comes back equal");
    }
    {   // ServerSendMailCheat: MailData.CurrencyContainer and the rest of the flattened mail data
        const schema::RpcSchema* rpc = pc.rpc("ServerSendMailCheat");
        CHECK(rpc != nullptr, "ServerSendMailCheat is a PlayerController call");
        MailPersistenceData mail = mail_sample_persistence();
        RpcParams p;
        CHECK(set_rpc_param(*rpc, p, "MailData.ExpirationDurationMs", Value::i64(mail.expiration_time)) &&
                  set_rpc_param(*rpc, p, "MailData.NodeId", Value::i64(mail.node)) &&
                  set_rpc_param(*rpc, p, "MailData.Sender", Value::str(mail.sender)) &&
                  set_rpc_param(*rpc, p, "MailData.Message.Title", Value::str(mail.message.title)) &&
                  set_rpc_param(*rpc, p, "MailData.Message.LocalizedTitle", Value(mail.message.localized_title)) &&
                  set_rpc_param(*rpc, p, "MailData.Message.Body", Value::str(mail.message.body)) &&
                  set_rpc_param(*rpc, p, "MailData.Message.LocalizedMessage", Value(mail.message.localized_message)) &&
                  set_rpc_param(*rpc, p, "MailData.Message.bSystem", Value::boolean(mail.message.system)) &&
                  set_rpc_param(*rpc, p, "MailData.AttachedItems", Value(ArrayValue{})),
              "the flattened mail data is set");
        CHECK(set_rpc_param(*rpc, p, "MailData.CurrencyContainer", Value(mail.currency)),
              "MailData.CurrencyContainer takes an FCurrencyContainer");
        BunchWriter out;
        bool failed = false;
        write_rpc_params(*rpc, p, out, {}, &failed);
        CHECK(!failed && out.bit_pos() > 0, "the call writes");
        size_t pos = 0;
        const auto back = read_rpc_params(*rpc, out.data(), out.bit_pos(), pos);
        CHECK(back.has_value() && pos == out.bit_pos(), "the call reads back to its last bit");
        const std::string* sender = back && back->values.count(4) ? back->values.at(4).get_if<std::string>() : nullptr;
        const CurrencyContainer* currency = back && back->values.count(11) ? back->values.at(11).get_if<CurrencyContainer>() : nullptr;
        CHECK(sender && *sender == mail.sender && currency && *currency == mail.currency,
              "the sender and the currency container come back equal");
    }
}

// ---- the other call that names FCurrencyContainer ---------------------------------

static void test_trade_currency() {
    std::printf("the trade calls' money\n");
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    CurrencyContainer money;
    money.entries.push_back(CurrencyEntry{0x1111222233334444ll, 0x5555666677778888ll, 0x4059000000000000ll});
    {
        // ServerUpdateTradeItems: TradeGuid, TradeItems (its Money is the FCurrencyContainer at the named place)
        const schema::RpcSchema* rpc = pc.rpc("ServerUpdateTradeItems");
        CHECK(rpc != nullptr, "ServerUpdateTradeItems is a PlayerController call");
        RpcParams p;
        CHECK(set_rpc_param(*rpc, p, "TradeGuid.A", Value::i64(0x11111111)) &&
                  set_rpc_param(*rpc, p, "TradeGuid.B", Value::i64(0x22222222)) &&
                  set_rpc_param(*rpc, p, "TradeGuid.C", Value::i64(0x33333333)) &&
                  set_rpc_param(*rpc, p, "TradeGuid.D", Value::i64(0x44444444)),
              "the trade's guid is set");
        CHECK(set_rpc_param(*rpc, p, "TradeItems.Items", Value(ArrayValue{})), "the trade's items are empty");
        CHECK(set_rpc_param(*rpc, p, "TradeItems.Money", Value(money)), "TradeItems.Money takes an FCurrencyContainer");
        CHECK(set_rpc_param(*rpc, p, "TradeItems.Version", Value::i64(3)), "the trade's version");
        BunchWriter out;
        bool failed = false;
        write_rpc_params(*rpc, p, out, {}, &failed);
        CHECK(!failed && out.bit_pos() > 0, "the call writes");
        size_t pos = 0;
        const auto back = read_rpc_params(*rpc, out.data(), out.bit_pos(), pos);
        CHECK(back.has_value() && pos == out.bit_pos(), "the call reads back to its last bit");
        const CurrencyContainer* money_back = back && back->values.count(6) ? back->values.at(6).get_if<CurrencyContainer>() : nullptr;
        CHECK(money_back && *money_back == money, "TradeItems.Money comes back equal");
    }
    {
        // ClientUpdateTrade: CharacterGuid, Items (its Money is the FCurrencyContainer at the named place)
        const schema::RpcSchema* rpc = pc.rpc("ClientUpdateTrade");
        CHECK(rpc != nullptr, "ClientUpdateTrade is a PlayerController call");
        RpcParams p;
        CHECK(set_rpc_param(*rpc, p, "CharacterGuid.A", Value::i64(0x11111111)) &&
                  set_rpc_param(*rpc, p, "CharacterGuid.B", Value::i64(0x22222222)) &&
                  set_rpc_param(*rpc, p, "CharacterGuid.C", Value::i64(0x33333333)) &&
                  set_rpc_param(*rpc, p, "CharacterGuid.D", Value::i64(0x44444444)),
              "the trade character's guid is set");
        CHECK(set_rpc_param(*rpc, p, "Items.Items", Value(ArrayValue{})), "the trade's items are empty");
        CHECK(set_rpc_param(*rpc, p, "Items.Money", Value(money)), "Items.Money takes an FCurrencyContainer");
        CHECK(set_rpc_param(*rpc, p, "Items.Version", Value::i64(3)), "the trade's version");
        BunchWriter out;
        bool failed = false;
        write_rpc_params(*rpc, p, out, {}, &failed);
        CHECK(!failed && out.bit_pos() > 0, "the call writes");
        size_t pos = 0;
        const auto back = read_rpc_params(*rpc, out.data(), out.bit_pos(), pos);
        CHECK(back.has_value() && pos == out.bit_pos(), "the call reads back to its last bit");
        const CurrencyContainer* money_back = back && back->values.count(6) ? back->values.at(6).get_if<CurrencyContainer>() : nullptr;
        CHECK(money_back && *money_back == money, "Items.Money comes back equal");
    }
}

static void test_mail_codecs() {
    test_mail_header();
    test_mail_header_expiry_field();
    test_mail_message();
    test_mail_item();
    test_mail_items_only();
    test_mail_currency();
    test_mail_several_items();
    test_mail_strings();
    test_localized_message_refused();
    test_mail_registry_and_refusals();
    test_mail_item_bad();
    test_mail_bounds();
    test_mail_status();
    test_mail_call_params();
    test_trade_currency();
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/mail_codec.h
//
//  The NetSerialize codecs of the mail structs (lane 13, piece P2): FMailHeader, FMailPersistenceData
//  with FMailMessage and FMailItem, and FCurrencyContainer.  codecs.h's read_struct / write_struct reach
//  the first, the persistence data and the currency container by name; the functions here are the same
//  codecs for a caller that holds a reader or a writer.  FMailMessage and FMailItem are plain members of
//  FMailPersistenceData and are not registered names.
//
//  Every layout is a bit stream, LSB first; a UBool is a bool as 32 bits, 0 or 1 (a value above 1 is
//  refused), an FString the 32-bit length including the NUL (0 empty) and its ANSI bytes or UTF-16 units
//  (codecs.h read_fstring), and an FLocalizedMessage its two FStrings and the argument lists
//  (callout_codec.h; a non-empty argument list is refused).
//
//  FMailHeader, in wire order:
//      128                 the mail's guid
//      32+8n               the sender as an FString
//      32+8n               the subject as an FString
//      32+8n + 32+8n + 32  the localized subject (FLocalizedMessage)
//      32                  the attachment flag (a UBool): the list shows its attachment icon when it is set
//      32                  the status (bit 4 read, bit 2 deleted)
//      64                  the expiration time (Unix ms; the list's countdown counts to it)
//      64                  the issue time (Unix ms)
//
//  FMailPersistenceData, in wire order:
//      128                 the mail's guid
//      128                 the sender's character guid (inferred)
//      128                 the recipient's character guid (inferred)
//      32+8n               the subject (inferred; P5 fills it with the message's title)
//      8                   the mail's type (its use is not known)
//      32                  the send batch
//      64                  when the mail was made (inferred)
//      64                  the expiration time (Unix ms)
//      64                  the node (inferred)
//      32+8n               the sender
//      FMailMessage        the message (below)
//      32 + n x FMailItem  the items: an int32 count, then each item (below)
//      FCurrencyContainer  the currency (below)
//      32                  whether the currency was claimed (a UBool)
//      32                  the status (bit 4 read, bit 2 deleted)
//
//  FMailMessage, in wire order: the title (32+8n), its localized form (32+8n + 32+8n + 32), the body
//  (32+8n), its localized form (32+8n + 32+8n + 32) and the system flag (32, a UBool).
//
//  FMailItem, in wire order: an FItemInstance (item_codec.h: one 0 bit empty, 1,096 bits plain) and the
//  claimed flag (32, a UBool).
//
//  FCurrencyContainer, in wire order: an int32 count and, per entry, the currency's record guid (64),
//  its record type id (64) and the amount (64: 8 bytes, held as a double as the warehouse cost map holds
//  it).  A TMap holds each key once, so the writer refuses a repeated (currency, type id).
//
//  kMaxMailItems and kMaxMailCurrencies are our bounds, not the game's.  A count is also checked against
//  the bits left, so a hostile count buys no memory.
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <cstddef>
#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// The most items one mail holds and the most currency entries its container holds (ours, not the game's).
constexpr size_t kMaxMailItems = 1024;
constexpr size_t kMaxMailCurrencies = 256;

/// True when write_mail_header writes `h` and read_mail_header reads it back equal: the strings fit an
/// FString and the localized subject has no argument list.
bool mail_header_fits(const MailHeader& h);

/// nullopt for a read past the end, a malformed string or a localized message with arguments.
std::optional<MailHeader> read_mail_header(wire::PacketReader& r);

/// False (writing nothing) when `h` does not fit.
bool write_mail_header(emit::BunchWriter& out, const MailHeader& h);

/// True when write_mail_message writes `m` and read_mail_message reads it back equal.
bool mail_message_fits(const MailMessage& m);

std::optional<MailMessage> read_mail_message(wire::PacketReader& r);

/// False (writing nothing) when `m` does not fit.
bool write_mail_message(emit::BunchWriter& out, const MailMessage& m);

/// True when write_mail_item writes `i`: the instance fits its own codec.
bool mail_item_fits(const MailItem& i);

std::optional<MailItem> read_mail_item(wire::PacketReader& r);

/// False (writing nothing) when `i` does not fit.
bool write_mail_item(emit::BunchWriter& out, const MailItem& i);

/// True when write_mail_persistence_data writes `d`: the strings fit, the message and the items fit, the
/// item and currency counts are within their bounds and no currency key repeats.
bool mail_persistence_data_fits(const MailPersistenceData& d);

std::optional<MailPersistenceData> read_mail_persistence_data(wire::PacketReader& r);

/// False (writing nothing) when `d` does not fit.
bool write_mail_persistence_data(emit::BunchWriter& out, const MailPersistenceData& d);

/// True when write_currency_container writes `c`: the entry bound holds and no (currency, type id) repeats.
bool currency_container_fits(const CurrencyContainer& c);

std::optional<CurrencyContainer> read_currency_container(wire::PacketReader& r);

/// False (writing nothing) when `c` does not fit.
bool write_currency_container(emit::BunchWriter& out, const CurrencyContainer& c);

}}}  // namespace aoc::protocol::repl

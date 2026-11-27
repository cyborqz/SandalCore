// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/market_codec.h  (the player marketplace: a price)
//
//  FCurrencyContainer as the wire carries it (a price of a listing, a mail's money, a trade's money): an int32 count, then per entry
//
//      int64   the currency tier's record guid (Money: 6064629127873626112)
//      int64   the currency tier's record type id (CurrencyTierDefRecordBase: 456318420933392641)
//      int64   the amount in the currency's base unit (copper for Money: 100 copper = 1 silver, 100 silver = 1 gold)
//
//  The layout was read from recorded prices: one entry, the Money tier, 1200 copper in one listing and 2000 in another, and the
//  amount is an integer (unlike the warehouse's cost, whose value is a double).  A price names its unit, so a container of another
//  currency reads the same way.  The one CurrencyContainer of value.h is shared with mail: mail_codec.h reads and writes it.
// ============================================================================
#pragma once

#include "protocol/repl/mail_codec.h"   // read_currency_container, write_currency_container, currency_container_fits

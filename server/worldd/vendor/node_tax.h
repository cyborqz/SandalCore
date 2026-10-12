// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  vendor/node_tax.h  (plan E1, stage 1, fix 4: the vendor window's tax table)
//
//  The vendor window of the client shows nothing until the server has answered its ServerGetNodeTaxTable.  The exe's
//  UVendorMenu::InitializeVendor asks for the table of the vendor's node (the NodeRecordGuid of the NPC's UAoCNodePlotComponent) when the
//  window opens, and only UVendorMenu::HandleReceiveNodeTaxDetails, which the server's ClientSetNodeTaxTable calls, fills the listings
//  (PopulateList), the tax labels and the funds (OnUpdateCurrency).  Retail answers within a packet or so with the node's whole table:
//  78 entries, one per tag of the NodeTax tree ("NodeTax.GoodsTax.MarketplaceTax"), each with the TaxDetails record its limits come from,
//  the node's current percent and the owning node's percent (captures 716 and 1536; libaocnet protocol/repl/vendor_codec.h).
//
//  What worldd answers: the same 78 tags with the values of a node nobody has changed (capture 1536's: vendor/node_tax_table.h, generated
//  by tools/capture/gen_vendor.py), but with every tax percent 0: the GM vendors are free and untaxed (modelled, not retail: a retail node
//  with no owner taxes at the 0.5 default).  The PTR vendor defs have no TaxTag at all ("None"), so the window looks no entry up and shows 0%
//  whatever the table holds; the 0 only keeps the table honest should a def with a tag ever be used.  The table's NodeGuid is the node the
//  client asked for.
// ============================================================================
#pragma once

#include "protocol/repl/value.h"

#include <cstdint>

namespace aoc::worldd::vendor {

/// The answer to ServerGetNodeTaxTable(node): every tax 0%.
protocol::repl::NodeTaxDetails untaxed_table(int64_t node);

}  // namespace aoc::worldd::vendor

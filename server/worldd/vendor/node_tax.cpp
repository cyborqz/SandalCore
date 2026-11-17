// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  vendor/node_tax.cpp  (see node_tax.h)
// ============================================================================
#include "vendor/node_tax.h"

#include "vendor/node_tax_table.h"

namespace aoc::worldd::vendor {

protocol::repl::NodeTaxDetails untaxed_table(int64_t node) {
    protocol::repl::NodeTaxDetails d;
    d.node = node;
    d.entries.reserve(tax_table::kRowCount);
    for (const tax_table::Row& row : tax_table::kRows) {
        protocol::repl::NodeTaxEntry e;
        e.tag = row.tag;
        e.details = row.details;
        e.flat_tax = 0;
        e.tax_percent = 0.0f;
        e.owning_node_percent = row.owning_node_percent;
        e.flag_a = row.flag_a;
        e.flag_b = row.flag_b;
        d.entries.push_back(std::move(e));
    }
    return d;
}

protocol::repl::NodeTaxDetails default_table(int64_t node) {
    protocol::repl::NodeTaxDetails d;
    d.node = node;
    d.entries.reserve(tax_table::kRowCount);
    for (const tax_table::Row& row : tax_table::kRows) {
        protocol::repl::NodeTaxEntry e;
        e.tag = row.tag;
        e.details = row.details;
        e.flat_tax = row.flat_tax;
        e.tax_percent = row.tax_percent;
        e.owning_node_percent = row.owning_node_percent;
        e.flag_a = row.flag_a;
        e.flag_b = row.flag_b;
        d.entries.push_back(std::move(e));
    }
    return d;
}

protocol::repl::NodeTaxDetails node_table(int64_t node, const std::map<std::string, world::TaxOverride>* overrides) {
    protocol::repl::NodeTaxDetails d = default_table(node);
    if (!overrides) return d;
    for (protocol::repl::NodeTaxEntry& e : d.entries) {
        const auto it = overrides->find(e.tag);
        if (it == overrides->end()) continue;
        e.tax_percent = it->second.percent;
        e.flat_tax = it->second.flat;
        e.flag_a = e.flag_b = true;
    }
    return d;
}

}  // namespace aoc::worldd::vendor

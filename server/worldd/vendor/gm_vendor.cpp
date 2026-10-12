// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  vendor/gm_vendor.cpp
//
//  See gm_vendor.h.
// ============================================================================
#include "vendor/gm_vendor.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::vendor {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string after_prefix(const std::string& def_name) {
    const std::string p = kGmVendorPrefix;
    return def_name.rfind(p, 0) == 0 ? def_name.substr(p.size()) : def_name;
}

}  // namespace

std::string kind_of(const std::string& def_name) {
    const std::string rest = after_prefix(def_name);
    const size_t us = rest.find('_');
    return us == std::string::npos ? rest : rest.substr(0, us);
}

std::string label_of(const std::string& def_name) {
    std::string rest = after_prefix(def_name);
    std::replace(rest.begin(), rest.end(), '_', ' ');
    return "GM " + rest;
}

std::optional<GmVendors> GmVendors::load(const content::ContentDb& db, std::string& error) {
    auto defs = db.vendor_defs_with_prefix(kGmVendorPrefix, error);
    if (!defs) return std::nullopt;
    if (defs->empty()) {
        error = std::string("content.db has no vendor def named ") + kGmVendorPrefix + "* (the vendor tables are contentc version 7's: rebuild the file with this checkout's contentc)";
        return std::nullopt;
    }
    GmVendors out;
    for (const content::VendorDefRow& d : *defs) {
        GmVendor v;
        v.guid = d.guid;
        v.name = d.name;
        v.kind = kind_of(d.name);
        v.label = label_of(d.name);
        v.tabs = d.inventories.size();
        for (const content::VendorInventoryRow& inv : d.inventories) {
            for (const content::VendorListing& l : inv.listings) {
                if (l.hidden || l.item == 0) continue;
                v.items.insert(l.item);
                ++v.listings;
            }
        }
        out.all_items_.insert(v.items.begin(), v.items.end());
        out.by_guid_[v.guid] = out.vendors_.size();
        out.vendors_.push_back(std::move(v));
    }
    // mounts, stage 1: the mounts def sells every mount item (contentc version 11's mount table).  A file without the table does not stop the load: the
    // vendors are as they were and mount_error() says why the mounts are not added.
    {
        std::string merror;
        const auto mounts = db.mount_data(merror);
        if (!mounts) {
            out.mount_error_ = merror;
        } else {
            out.mounts_total_ = mounts->mounts.size();
            for (GmVendor& v : out.vendors_) {
                if (v.name != kMountsVendorName) continue;
                for (const content::MountRow& m : mounts->mounts) {
                    if (v.items.insert(m.item).second) {
                        ++out.mounts_added_;
                        ++v.listings;
                    }
                    out.all_items_.insert(m.item);
                }
            }
        }
    }
    auto gm = db.gm_items(error);
    if (!gm) return std::nullopt;
    out.gm_items_ = gm->size();
    for (const content::GmItemRow& r : *gm) {
        if (out.all_items_.count(r.guid)) ++out.gm_stocked_;
        else out.unstocked_.push_back(r.name);
    }
    return out;
}
// C | Y | B / CoA

GmVendors GmVendors::from_defs(std::vector<GmVendor> defs) {
    GmVendors out;
    for (GmVendor& v : defs) {
        out.all_items_.insert(v.items.begin(), v.items.end());
        out.by_guid_[v.guid] = out.vendors_.size();
        out.vendors_.push_back(std::move(v));
    }
    return out;
}

const GmVendor* GmVendors::find(int64_t guid) const {
    const auto it = by_guid_.find(guid);
    return it == by_guid_.end() ? nullptr : &vendors_[it->second];
}

PurchasePlan GmVendors::plan_purchase(const content::ContentDb& db, const GmVendor& vendor, const protocol::repl::VendorPurchaseCart& cart, size_t free_slots) const {
    PurchasePlan plan;
    const auto refuse = [&](std::string why) {
        plan.ok = false;
        plan.refusal = std::move(why);
        plan.stacks.clear();
        return plan;
    };
    if (cart.vendor != vendor.guid) return refuse("the cart names vendor " + std::to_string(cart.vendor) + ", not the one the player has open (" + vendor.name + ")");
    if (cart.items.empty()) return refuse("the cart is empty");
    if (cart.items.size() > kMaxCartItems) return refuse("the cart holds " + std::to_string(cart.items.size()) + " items (at most " + std::to_string(kMaxCartItems) + ")");
    std::string error;
    size_t total_stacks = 0;
    for (const protocol::repl::VendorCartItem& e : cart.items) {
        if (e.quantity < 1 || e.quantity > kMaxQuantity) return refuse("an item of the cart asks for a quantity of " + std::to_string(e.quantity));
        if (!vendor.items.count(e.item_record)) return refuse("item " + std::to_string(e.item_record) + " is not one of " + vendor.name + "'s listings");
        const auto rec = db.item(e.item_record, error);
        if (!rec) return refuse(error.empty() ? "content.db has no item record " + std::to_string(e.item_record) : "content.db failed: " + error);
        const int32_t stack = rec->max_stack_size > 0 ? rec->max_stack_size : 1;
        const size_t stacks = static_cast<size_t>((static_cast<int64_t>(e.quantity) + stack - 1) / stack);
        total_stacks += stacks;
        if (total_stacks > free_slots) {
            return refuse("the bag has " + std::to_string(free_slots) + " free slot(s) and the cart needs more (" + std::to_string(total_stacks) + " stack(s) so far)");
        }
        int32_t left = e.quantity;
        while (left > 0) {
            use::Granted g;
            g.record = e.item_record;
            g.quantity = left < stack ? left : stack;
            g.quality = rec->default_quality;
            g.durability = static_cast<float>(std::floor(rec->max_durability + 0.5));
            plan.stacks.push_back(g);
            left -= g.quantity;
        }
    }
    plan.ok = true;
    plan.summary = std::to_string(cart.items.size()) + " item(s) in " + std::to_string(plan.stacks.size()) + " stack(s) from " + vendor.label;
    return plan;
}

std::vector<GmPlacement> gm_layout(size_t count, const spawns::AreaConfig& area, double facing_degrees, double z) {
    // two arcs in front of the spot: radius 8 m for the first 12, 12.5 m for the rest, 140 and 160 degrees wide
    constexpr size_t kInner = 12;
    struct Row { double radius, span_degrees; };
    const Row rows[2] = {{800.0, 140.0}, {1250.0, 160.0}};
    std::vector<GmPlacement> out;
    for (size_t i = 0; i < count; ++i) {
        const size_t row = i < kInner ? 0 : 1;
        const size_t first = row == 0 ? 0 : kInner;
        const size_t n = row == 0 ? std::min(count, kInner) : count - kInner;
        const size_t k = i - first;
        const double t = n <= 1 ? 0.0 : static_cast<double>(k) / static_cast<double>(n - 1) - 0.5;   // -0.5 .. +0.5 across the arc
        const double angle = facing_degrees + t * rows[row].span_degrees;
        const double rad = angle * kPi / 180.0;
        GmPlacement p;
        p.x = area.center_x + rows[row].radius * std::cos(rad);
        p.y = area.center_y + rows[row].radius * std::sin(rad);
        p.z = z;
        p.yaw_degrees = std::fmod(angle + 180.0, 360.0);   // faces the spot
        out.push_back(p);
    }
    return out;
}

}  // namespace aoc::worldd::vendor

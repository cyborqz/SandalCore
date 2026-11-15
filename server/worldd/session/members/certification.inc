// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/certification.inc: the members of PlayerSession that the artisan certification and the recipe items added
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// Whether the player may take the certification action of `profession` (a Promote one tier up, else a Demote one down): a Promote needs the profession at its tier's cap
    /// and room under the next tier's MaximumAllowable, a Demote a tier to go down from.  False, with `why`, for a choice the player may not take (the line changes nothing).
    bool certification_allowed(int64_t profession, bool promote, std::string& why) const;
    /// Applies the certification actions a line of the conversation took: each profession moves one tier, the XPTracker is sent with the new certification (the client's
    /// LevelingCertificationsFastMap), the levels gained are counted and the progress is handed over.  The choices were checked with certification_allowed first.
    void apply_certifications(Connection& conn, const std::vector<dialogue::Certification>& list);
    /// A LearnRecipes item the plan accepted: its recipes join the character's known list (saved with the inventory), the item is consumed as the data says and the client's
    /// OwnerInfo is sent the list.  Empty on success, else the reason the use is refused (nothing changes then).
    std::string learn_recipes_from_use(Connection& conn, const use::Plan& plan, const protocol::repl::ItemUse& use, const svc::InventoryItem& item);
private:

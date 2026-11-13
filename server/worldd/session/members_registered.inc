// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members_registered.inc: the member fragments of the features added after the split
//  A new feature of PlayerSession puts its declarations (public accessors, private functions, data members) into a file of its own, session/members/<feature>.inc, and adds ONE
//  line here at the END: #include "session/members/<feature>.inc".  The file is included at the end of the class, so its members come after all the older ones; it sets its
//  own `public:` and `private:` and ends with `private:`.  Two branches that add a line at the same time both keep it (the merge=union rule of .gitattributes), so this list does
//  not conflict.  The order of the lines does not matter.
// ============================================================================
#include "session/members/gather.inc"
#include "session/members/craft.inc"
#include "session/members/move_ack.inc"
#include "session/members/pets.inc"
#include "session/members/based_move.inc"
#include "session/members/view_phases.inc"
#include "session/members/presence.inc"
#include "session/members/chests.inc"
#include "session/members/citizenship.inc"
#include "session/members/node_taxes.inc"
#include "session/members/surface_move.inc"
#include "session/members/chat.inc"
#include "session/members/ignore.inc"
#include "session/members/emotes.inc"
#include "session/members/node_inventory.inc"
#include "session/members/buy_orders.inc"
#include "session/members/reputation.inc"
#include "session/members/node_xp.inc"

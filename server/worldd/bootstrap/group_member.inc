// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  bootstrap/group_member.inc: one member of a party as ClientUpdateGroup's squad element carries it (social step 8; live Health and Mana)
//
//  The element's commands in the generated schema's order (handles 1 to 16): the name, the character GUID, the class, the online flag, and the nine PlayerStats values
//  (the health and mana shares, the maxima, the level, the alive state, the place, the map marker and the heading).  The same values are the SocialComponent's
//  PlayerStatsReplication item (bootstrap/bootstrap_social.inc).
// ============================================================================

repl::ElementState group_member_element(const GroupMember& shown) {
    repl::ElementState member;
    member.values[1] = Value::str(shown.name);
    for (int i = 0; i < 4; ++i) member.values[static_cast<uint16_t>(2 + i)] = Value::i64(static_cast<int32_t>(shown.guid[i]));
    member.values[6] = Value::u64(static_cast<uint64_t>(shown.primary_class));
    member.values[7] = Value::boolean(true);   // bIsOnline: the session is in the group
    member.values[8] = Value::f32(shown.health_percent);
    member.values[9] = Value::f32(shown.max_health);
    member.values[10] = Value::f32(shown.mana_percent);
    member.values[11] = Value::f32(shown.max_mana);
    member.values[12] = Value::i64(shown.level);
    member.values[13] = Value::u64(shown.alive_status);
    member.values[14] = Value(shown.location);
    member.values[15] = Value(shown.map_marker);
    member.values[16] = Value::f32(shown.heading);
    return member;
}

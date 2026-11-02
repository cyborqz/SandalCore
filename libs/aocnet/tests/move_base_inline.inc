// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/move_base_inline.inc  (included by test_repl_move_packed.cpp, before its main)
//
//  A ServerMovePacked whose MovementBase is named in place (GUID ObjectId 1, export flags, the outer, the path): the client has no GUID for a component the server never
//  exported.  The reader used to refuse the whole move ("MovementBase is not an object reference the reader knows"), and a pawn that stood on such a base lost every move.
//  Now it reads, keeps the rest of the move, and writes back to the same bits.
// ============================================================================

static void test_move_base_inline() {
    std::printf("a movement base named in place\n");
    FIntrepidNetworkGUID horse;
    horse.ObjectId = 17045704;
    horse.ServerId = 60;
    horse.Randomizer = 0x5a5a5a5a;
    InlineObject cylinder;
    cylinder.flags = 5;   // a path and its checksum
    cylinder.outer = horse;
    cylinder.path = "CollisionCylinder";
    cylinder.checksum = 0xb876932f;

    MovePacked p;
    p.new_move = walking_move(20.5f, 12.25, -30.5, 197.0);
    p.new_move.base_inline = cylinder;
    p.pending = walking_move(20.48f, 12.0, -30.0, 197.0);
    p.pending->base_inline = cylinder;
    p.old = walking_move(20.46f, 11.5, -29.5, 197.0);
    p.old->base = horse;   // a plain reference beside it
    size_t bits = 0;
    const auto data = written(p, &bits);
    CHECK(bits > 0, "a move on a base named in place writes");
    std::string why;
    const auto r = read_move_packed(data.data(), data.size(), 0, bits, &why);
    CHECK(r.has_value(), "and it reads (an inline base was the one thing the reader refused)");
    if (!r) {
        std::printf("    (%s)\n", why.c_str());
        return;
    }
    CHECK(r->new_move.base_inline && *r->new_move.base_inline == cylinder && !r->new_move.base && r->new_move.has_base(),
          "the new move keeps the base: ObjectId 1's flags, outer, path and checksum");
    CHECK((r->new_move.location_cm() == Vec3d{12.25, -30.5, 197.0}) && r->new_move.time_stamp == 20.5f && r->new_move.control_rotation.yaw == 16384,
          "and the rest of the move: the location (relative to the base), the time stamp, the view");
    CHECK(r->pending && r->pending->base_inline && *r->pending->base_inline == cylinder && r->old && r->old->base && *r->old->base == horse && !r->old->base_inline,
          "the pending move and the old move keep theirs (one named in place, one a plain reference)");
    CHECK(*r == p, "the whole parameter reads back equal");
    size_t again_bits = 0;
    const auto again = written(*r, &again_bits);
    CHECK(again_bits == bits && same_bit_range(again.data(), data.data(), bits), "and writes back to the same bits");
    CHECK(describe_base(r->new_move) == "\"CollisionCylinder\" of object 17045704" && describe_base(*r->old) == "object 17045704" && describe_base(walking_move(1, 0, 0, 0)) == "none",
          "describe_base says which object the base is (for the log)");

    // a bone beside an inline base, and the refusals
    p.new_move.base_bone = NameValue::plain("spine_01", 3);
    const auto with_bone = written(p, &bits);
    const auto rb = read_move_packed(with_bone.data(), with_bone.size(), 0, bits);
    CHECK(rb && rb->new_move.base_bone && rb->new_move.base_bone->text == "spine_01" && rb->new_move.base_inline, "an inline base with a bone name reads too");
    MovePacked both;
    both.new_move = walking_move(1.0f, 0, 0, 0);
    both.new_move.base = horse;
    both.new_move.base_inline = cylinder;
    BunchWriter w;
    CHECK(!write_move_packed(w, both) && w.bit_pos() == 0, "a move with a plain base and an inline one is not written (a move has one base)");
    MovePacked broken;
    broken.new_move = walking_move(1.0f, 0, 0, 0);
    broken.new_move.base_inline = cylinder;
    broken.new_move.base_inline->flags = 4;   // a checksum without a path: Unreal reads the checksum with the path only
    BunchWriter w2;
    CHECK(!write_move_packed(w2, broken) && w2.bit_pos() == 0, "an inline base with a checksum and no path is not written");
}

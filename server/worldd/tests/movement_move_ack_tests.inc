// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_move_ack_tests.inc: the answers to a player's good moves at the original server's pace (session/player_move_ack.cpp)
//
//  The original server did not answer every move: the latest good move of a frame was answered when more than 100 ms had gone by since its last answer, otherwise
//  the answer was dropped (about every second move of a walking client was answered; one that moved less than 10 times a second got an answer for each).
//  The checks run the real session through its tick on 33 ms frames: the moves a bot-like client sends 36 times a second, and 15 times a second; a refused move; a
//  move whose answer the gap held back.  They need the pace set (PlayerWorld::set_move_ack_gap): the default of the world, 0, answers every move at once, which the
//  older checks of this program rely on.
// ============================================================================
#include <optional>

namespace move_ack_checks {

/// The TimeStamp of a delivered ClientMoveResponsePacked (the read of is_ack), nullopt for any other chain.
inline std::optional<float> ack_stamp(const fake::Delivered& d) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!d.exports.empty() || d.reliable || d.content.bit_pos() != 68 || !blocks || blocks->size() != 1) return std::nullopt;
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    size_t pos_bits = (*blocks)[0].payload_pos;
    const auto h = repl::read_field_header(pawn, d.content.data(), pos_bits + (*blocks)[0].payload_bits, pos_bits);
    if (!h || std::string(pawn.field(h->field)->name) != "ClientMoveResponsePacked") return std::nullopt;
    aoc::protocol::wire::PacketReader r(d.content.data(), d.content.byte_size(), pos_bits + h->payload_bits);
    r.set_pos(pos_bits);
    if (r.read_bits(10) != 645) return std::nullopt;
    return std::bit_cast<float>(static_cast<uint32_t>(r.read_bits(32)));
}

/// One frame of `ms`: the move (when there is one) arrives and is judged, then the server's tick runs and sends.  The TimeStamps of the answers the tick sent.
inline std::vector<float> frame(Harness& h, const std::optional<repl::NetworkMove>& move, milliseconds ms = milliseconds(33)) {
    if (move) h.client_sends({h.client.actor(h.pawn_ch, client_move(*move), false)});
    const size_t from = h.got.size();
    h.step(ms);
    std::vector<float> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pawn_ch) continue;
        if (const auto ts = ack_stamp(h.got[i])) out.push_back(*ts);
    }
    return out;
}

}  // namespace move_ack_checks

void test_move_acks_at_the_original_pace() {
    using namespace move_ack_checks;
    std::printf("move answers: a move every frame (36 a second) is answered every fourth frame, with the latest move's TimeStamp\n");
    Harness h;
    CHECK(h.fx.world->move_ack_gap() == Clock::duration::zero(), "(the world answers every move at once unless a pace is set: worldd sets 100 ms)");
    h.fx.world->set_move_ack_gap(milliseconds(100));
    float ts = 1.0f;
    double x = kX;
    std::vector<float> sent, answered;
    for (int i = 0; i < 40; ++i) {
        ts += 0.033f;
        x += 13.2;
        sent.push_back(ts);
        for (const float a : frame(h, move_at(ts, x, kY, kZ))) answered.push_back(a);
    }
    CHECK(answered.size() == 10, "40 moves 33 ms apart: 10 answers (the first at once, then one more than 100 ms after the last: every fourth frame)");
    bool latest = answered.size() == 10;
    for (size_t k = 0; latest && k < answered.size(); ++k) latest = answered[k] == sent[4 * k];
    CHECK(latest, "each answer carries the TimeStamp of the move that arrived in its own frame: the 1st, 5th, 9th ... of the 40");
    CHECK(h.s->moves_acknowledged() == 10 && h.s->move_acks_held_back() == 30, "counted: 10 answered, 30 not");
    CHECK(h.s->moves_refused() == 0 && h.s->moves_stale() == 0 && h.s->corrections_sent() == 0, "no move is refused, stale or corrected for it");
    const GoodPosition& g = h.s->movement().good();
    CHECK(near(g.x, x) && g.time_stamp == ts, "the last good position is the last move's: every move is judged, answered or not");

    std::printf("move answers: a move every second frame (15 a second) is answered every second move, as a walking client was\n");
    Harness w;
    w.fx.world->set_move_ack_gap(milliseconds(100));
    ts = 1.0f;
    x = kX;
    sent.clear();
    answered.clear();
    for (int i = 0; i < 40; ++i) {
        std::optional<repl::NetworkMove> move;
        if (i % 2 == 0) {
            ts += 0.066f;
            x += 26.4;
            sent.push_back(ts);
            move = move_at(ts, x, kY, kZ);
        }
        for (const float a : frame(w, move)) answered.push_back(a);
    }
    bool every_second = answered.size() == 10;
    for (size_t k = 0; every_second && k < answered.size(); ++k) every_second = answered[k] == sent[2 * k];
    CHECK(sent.size() == 20 && every_second, "20 moves 66 ms apart: 10 answers, for the 1st, 3rd, 5th ... (the original server's way)");
    CHECK(w.s->moves_acknowledged() == 10 && w.s->move_acks_held_back() == 10 && w.s->moves_refused() == 0, "counted, none refused");

    std::printf("move answers: a client that moves less than ten times a second is answered for every move\n");
    Harness slow;
    slow.fx.world->set_move_ack_gap(milliseconds(100));
    ts = 1.0f;
    x = kX;
    sent.clear();
    answered.clear();
    for (int i = 0; i < 30; ++i) {
        std::optional<repl::NetworkMove> move;
        if (i % 4 == 0) {
            ts += 0.132f;
            x += 52.8;
            sent.push_back(ts);
            move = move_at(ts, x, kY, kZ);
        }
        for (const float a : frame(slow, move)) answered.push_back(a);
    }
    CHECK(sent.size() == 8 && answered == sent, "8 moves 132 ms apart: 8 answers, each its own move's");
}
AOC_REGISTER_TEST(test_move_acks_at_the_original_pace);

void test_move_ack_held_back_is_dropped_not_carried() {
    using namespace move_ack_checks;
    std::printf("move answers: an answer the gap holds back is dropped; it is not sent later when the client stops moving\n");
    Harness h;
    h.fx.world->set_move_ack_gap(milliseconds(100));
    const auto first = frame(h, move_at(1.033f, kX + 13.2, kY, kZ));
    CHECK(first.size() == 1 && first[0] == 1.033f, "the first move is answered at once (at its frame's tick)");
    const auto second = frame(h, move_at(1.066f, kX + 26.4, kY, kZ));
    CHECK(second.empty(), "the next frame's move is 33 ms after the answer: held back");
    size_t later = 0;
    for (int i = 0; i < 8; ++i) later += frame(h, std::nullopt).size();
    CHECK(later == 0 && h.s->moves_acknowledged() == 1 && h.s->move_acks_held_back() == 1, "and it never goes out: the client keeps the move saved until a later answer frees it");
    const auto third = frame(h, move_at(1.3f, kX + 39.6, kY, kZ));
    CHECK(third.size() == 1 && third[0] == 1.3f, "the next move, long after the last answer, is answered with its own TimeStamp (which frees the held one too)");
}
AOC_REGISTER_TEST(test_move_ack_held_back_is_dropped_not_carried);

void test_move_ack_yields_to_a_correction() {
    using namespace move_ack_checks;
    std::printf("move answers: a refused move is corrected at once, and the answer held for the move before it is dropped\n");
    Harness h;
    h.fx.world->set_move_ack_gap(milliseconds(100));
    frame(h, move_at(1.033f, kX + 13.2, kY, kZ));
    h.client_sends({h.client.actor(h.pawn_ch, client_move(move_at(1.066f, kX + 26.4, kY, kZ)), false)});   // judged and held
    const GoodPosition good = h.s->movement().good();
    const size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, client_move(move_at(1.083f, kX + 5026.4, kY, kZ)), false)});   // 50 m in 17 ms: refused
    h.flush();
    std::vector<const fake::Delivered*> replies;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel == h.pawn_ch) replies.push_back(&h.got[i]);
    }
    CHECK(replies.size() == 1 && correction_is(*replies[0], 1.083f, good.x, good.y, good.z), "the refusal's ClientAdjustPosition goes out with the datagram, not at the next tick");
    const auto after = frame(h, std::nullopt);
    CHECK(after.empty() && h.s->moves_acknowledged() == 1 && h.s->moves_refused() == 1 && h.s->corrections_sent() == 1,
          "and the answer held for the good move before it is not sent (the correction wins, as the original server's pending adjustment)");
}
AOC_REGISTER_TEST(test_move_ack_yields_to_a_correction);

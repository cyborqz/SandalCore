// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "protocol/wire/packet_reader.h"
#include <array>
#include <cstdio>
#include <limits>
#include <vector>

using aoc::protocol::wire::PacketReader;
static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("[FAIL] %s\n", msg); ++failures; } } while (0)

int main() {
    const uint8_t ones[] = {0xff, 0xff};
    PacketReader short_int(ones, sizeof(ones), 1);
    CHECK(short_int.read_serialize_int(16) == 0 && short_int.overflowed(),
          "SerializeInt rejects reads across the effective bit limit");
    PacketReader zero_width(nullptr, 0);
    CHECK(zero_width.read_serialize_int(1) == 0 && !zero_width.overflowed(),
          "SerializeInt of a one-value range consumes no bits");
    PacketReader width(ones, sizeof(ones));
    CHECK(width.read_bits(65) == 0 && width.overflowed() && width.pos() == 0,
          "reads wider than uint64 are rejected without undefined shifts");
    PacketReader bytes(ones, sizeof(ones));
    CHECK(bytes.read_uint(std::numeric_limits<int>::max()) == 0 && bytes.overflowed(),
          "byte width is checked before multiplication");
    PacketReader position(ones, sizeof(ones));
    position.set_pos(std::numeric_limits<size_t>::max());
    CHECK(position.read_bits(8) == 0 && position.overflowed(),
          "cursor overflow cannot wrap back into the buffer");
    PacketReader effective(ones, sizeof(ones), 100);
    CHECK(effective.effective_bits() == sizeof(ones) * 8 && effective.overflowed(),
          "effective bits cannot exceed the backing buffer");
    PacketReader null_buffer(nullptr, 8, 64);
    CHECK(null_buffer.read_bits(1) == 0 && null_buffer.overflowed(), "null backing data is rejected");

    std::array<uint8_t, 10> sip{};
    size_t off = 0;
    ue5::write_sip(sip.data(), sip.size(), off, std::numeric_limits<uint64_t>::max());
    PacketReader max_sip(sip.data(), sip.size());
    CHECK(max_sip.read_sip() == std::numeric_limits<uint64_t>::max() && !max_sip.overflowed(),
          "maximum uint64 varint round-trips");
    sip.back() = 4;
    PacketReader overflow_sip(sip.data(), sip.size());
    CHECK(!overflow_sip.read_sip() && overflow_sip.overflowed() && overflow_sip.pos() == 0,
          "varint with more than 64 payload bits is rejected");
    sip.fill(1);
    PacketReader unterminated_sip(sip.data(), sip.size());
    CHECK(!unterminated_sip.read_sip() && unterminated_sip.overflowed(),
          "varint continuation in the tenth byte is rejected");
    const uint8_t continuation[] = {1};
    PacketReader truncated_sip(continuation, sizeof(continuation));
    CHECK(!truncated_sip.read_sip() && truncated_sip.pos() == 0,
          "truncated varint restores its cursor");
// -- C O A x Cyb --

    // A counted FString with an early NUL must not consume the following field.
    const uint8_t ascii[] = {4, 0, 0, 0, 'a', 0, 'b', 0, 0x5a};
    PacketReader text(ascii, sizeof(ascii));
    CHECK(text.read_fstring() == std::string("a\0b", 3) && text.read_uint8() == 0x5a,
          "ASCII FString consumes its declared length exactly");
    const uint8_t wide[] = {0xfc, 0xff, 0xff, 0xff, 'a', 0, 0, 0, 'b', 0, 0, 0, 0x5a};
    PacketReader wide_text(wide, sizeof(wide));
    CHECK(wide_text.read_fstring() == std::string("a\0b", 3) && wide_text.read_uint8() == 0x5a,
          "UCS-2 FString consumes its declared length exactly");
    const uint8_t no_null[] = {2, 0, 0, 0, 'a', 'b'};
    PacketReader unterminated_text(no_null, sizeof(no_null));
    CHECK(!unterminated_text.read_fstring(), "FString requires the declared trailing NUL");
    PacketReader truncated_text(ascii, sizeof(ascii), 7 * 8);
    CHECK(!truncated_text.read_fstring() && truncated_text.overflowed(),
          "truncated FString is rejected before reading its characters");
    const uint8_t min_length[] = {0, 0, 0, 0x80};
    PacketReader impossible_text(min_length, sizeof(min_length));
    CHECK(!impossible_text.read_fstring(), "minimum signed length cannot overflow when negated");

    std::printf("packet_reader: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

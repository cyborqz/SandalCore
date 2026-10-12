// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  capture_packets.h
//
//  Packets from fixtures/e2e/capture_20260928_061401.jsonl (the retail client
//  against the legacy server, character cyb), as hex, for the connection tests.
//  Row numbers count the capture rows that carry packet bytes.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace capture {

// row 0, C>S, 148 bytes: client Initial (character cyb, join ticket)
inline const char* kInitial =
    "96760c50a80102800027b117520000000000000000000000000000000000000000000000"
    "000000000000007f7f7f7fff7f00000000000000000000000000000000865cecbbe51f10"
    "ebf0878cf3d0e722fa00000000000000000000000000000000ac8edc9dfcc25c8052df75"
    "3ec81346b70000000000000000000000000000000000000000000000009f2f1634e56169"
    "9b346e01";

// row 1, S>C, 153 bytes: server Challenge
inline const char* kChallenge =
    "96760c50a80182800027b117520080c32973f38d3b5240b937febd9e1d1d234e6380280e"
    "407c83537abe973821c70d435d5ba5ede27aedd31e697e0fb207a107f1865cecbbe51f10"
    "ebf0878cf3d0e722fa12ced270e0b657f8c22f5fc3f0fe0a52ac8edc9dfcc25c8052df75"
    "3ec81346b70000000081bb27c16666666605d12241000000000028cf409f5ddc3799e3d1"
    "b42efc741ffdad0101";

// row 2, C>S, 154 bytes: client Response
inline const char* kResponse =
    "96760c50a80102010127b117520080c32973f38d3b5240b937febd9e1d1d234e6380280e"
    "407c83537abe973821c70d435d5ba5ede27aedd31e697e0fb207a107f1865cecbbe51f10"
    "ebf0878cf3d0e722fa12ced270e0b657f8c22f5fc3f0fe0a52ac8edc9dfcc25c8052df75"
    "3ec81346b70000000081bb27c16666666605d12241000000000028cf40c541e612d438ea"
    "e605c67753ae6ea69101";

// row 3, S>C, 147 bytes: server Ack
inline const char* kAck =
    "96760c50a80182010127b117520080000000000000f0bfb937febd9e1d1d234e6380280e"
    "407c83537abe973821c70d435d5ba5ede27aedd31e697e0fb207a107f1865cecbbe51f10"
    "ebf0878cf3d0e722fa12ced270e0b657f8c22f5fc3f0fe0a52ac8edc9dfcc25c8052df75"
    "3ec81346b70000000081bb27c16666666605d12241000000000028cf406bfc8c6f0f7205"
    "ed0601";

// row 4, C>S, 40 bytes: client NMT_Hello (opens channel 0)
inline const char* kClientHello =
    "96760c5008e0defe3d000000004ec871c350d7ff4d00fcf75f000c00043889bd90020000"
    "0000000c";
// Cyb captures, C-O-A code

// row 5, S>C, 42 bytes: server NMT_Challenge
inline const char* kServerNmtChallenge =
    "96760c5008f8f7b9f7ffffff3f4ec871c350d7ff1100baff178003030900000035363944"
    "394631300003";

// row 6, C>S, 157 bytes: client NMT_Login
inline const char* kClientLogin =
    "96760c5008e0deff3d000000004ec871c350d70a100000fe174020050200000030003700"
    "00003f4e616d653d4445534b544f502d4558414d504c322d323731313934443934364241"
    "37343433363943423539394237424238414334320008310000004445534b544f502d4558"
    "414d504c322d323731313934443934364241373434333639434235393942374242384143"
    "343200050000004e554c4c0003";

// row 7, S>C, 151 bytes: server NMT_Welcome
inline const char* kServerWelcome =
    "96760c5008fcf7baf7ffffff3f4ec871c350d7ff1100bbff17c01e01330000002f47616d"
    "652f4c6576656c732f56657272615f576f726c645f4d61737465722f56657272615f576f"
    "726c645f4d6173746572003b0000002f47616d652f47616d65426c75657072696e74732f"
    "416f4347616d654d6f64654261736542502e416f4347616d654d6f64654261736542505f"
    "43000000000003";

// row 8, C>S, 20 bytes: client packet with no bunches
inline const char* kClientAckOnly = "96760c5008e4de007e000000004ec871c3509701";

// row 9, C>S, 33 bytes: client NMT_NetSpeed
inline const char* kClientNetSpeed = "96760c5008e8de01fe000000004ec871c3505713100001fe17400104400d030003";

// row 10, S>C, 21 bytes: server keep-alive
inline const char* kServerKeepAlive = "96760c500804f8bbf7ffffff3f4ec871c350d7ff0d";

// row 18, C>S, 29 bytes: client NMT_Join
inline const char* kClientJoin = "96760c5008e8de02fe000000004ec871c350d7ff110002fe1740001206";

// row 3246, C>S, 29 bytes: client quits: control-channel close
inline const char* kClientClose = "96760c5008100b6ac7ffffff3f4ec871c350d73c1404c080ff05008001";

inline std::vector<uint8_t> bytes(const char* hex) {
    std::vector<uint8_t> out;
    const std::string s(hex);
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

}  // namespace capture

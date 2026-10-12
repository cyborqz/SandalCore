# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""Replay a captured client session at a running aoc_server.

Sends every C>S packet from a capture.jsonl (as written by ProxyLogger into
logs/run_*/capture.jsonl) to the game port, in order, then keeps listening
for --tail seconds and reports how many packets the server still sent.
The handshake cookie is not re-derived: the emulator logs cookie_match but
does not reject a mismatch, so the captured Initial/Response pair works.

Use alternate ports so a normal stack can keep running:

    dist/Release/aoc_server.exe --use-embedded-bootstrap --enable-live-world \
        --port 4443 --game-port 17777
    python tools/replay_capture.py logs/run_20260928_061401/capture.jsonl --port 17777

If the capture ends with the client's control-channel Close (a normal quit),
"after last packet" should be 0: the server must stop sending once the
client has closed the connection.
"""
import argparse
import datetime
import json
import socket
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--port", type=int, default=17777)
    ap.add_argument("--gap-ms", type=float, default=3.0, help="delay between packets")
    ap.add_argument("--realtime", action="store_true",
                    help="keep the capture's original timing instead of --gap-ms")
    ap.add_argument("--tail", type=float, default=3.0, help="seconds to listen after the last packet")
    ap.add_argument("--dump-rx", help="write received S>C packets as JSON lines (hex)")
    args = ap.parse_args()

    with open(args.capture) as f:
        rows = [j for j in map(json.loads, f) if j.get("dir") == "C>S" and "hex" in j]
    pkts = [bytes.fromhex(j["hex"]) for j in rows]
    stamps = [datetime.datetime.fromisoformat(j["ts"]).timestamp() for j in rows]

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.setblocking(False)
    dump = open(args.dump_rx, "w") if args.dump_rx else None

    def drain():
        n = 0
        while True:
            try:
                data, _ = sock.recvfrom(65536)
            except BlockingIOError:
                return n
            n += 1
            if dump:
                dump.write(json.dumps({"t": time.time(), "hex": data.hex()}) + "\n")

    received = 0
    for i, p in enumerate(pkts):
        sock.sendto(p, ("127.0.0.1", args.port))
        if i + 1 < len(pkts):
            gap = stamps[i + 1] - stamps[i] if args.realtime else args.gap_ms / 1000.0
            if i < 2:
                gap = max(gap, 0.05)  # let the handshake settle
            deadline = time.time() + gap
            while True:
                received += drain()
                left = deadline - time.time()
                if left <= 0:
                    break
                time.sleep(min(left, 0.01))
        received += drain()
# built with C.Y.B and C.O.A

    received += drain()
    after = 0
    deadline = time.time() + args.tail
    while time.time() < deadline:
        time.sleep(0.05)
        after += drain()

    if dump:
        dump.close()
    print(f"sent {len(pkts)} C>S packets, received {received} S>C packets during replay, "
          f"{after} after last packet ({args.tail:.1f}s)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Measure the adb-reverse link to the running GoVR client: RTT (PING/PONG) and bulk throughput.

Usage: linkbench.py [seconds]. The client must be running; no other server may be listening.
Bulk data is sent as message type 99, which the client reads and ignores.
"""
import socket, struct, sys, threading, time, subprocess

PORT, PING, PONG = 9950, 5, 67
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
subprocess.run(["adb", "reverse", f"tcp:{PORT}", f"tcp:{PORT}"], capture_output=True)
ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("127.0.0.1", PORT)); ls.listen(1)
s, _ = ls.accept(); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
rtts, lock = [], threading.Lock()

def rx():
    buf = b""
    while True:
        d = s.recv(65536)
        if not d: return
        buf += d
        while len(buf) >= 8:
            t, n = struct.unpack("<B3xI", buf[:8])
            if len(buf) < 8 + n: break
            if t == PONG:
                with lock: rtts.append((time.monotonic_ns() - struct.unpack("<Q", buf[8:16])[0]) / 1e6)
            buf = buf[8 + n:]
threading.Thread(target=rx, daemon=True).start()
time.sleep(1)
for _ in range(50):
    s.sendall(struct.pack("<B3xIQ", PING, 8, time.monotonic_ns())); time.sleep(0.02)
time.sleep(0.5)
r = sorted(rtts); print(f"idle RTT over adb: n={len(r)} min {r[0]:.2f} p50 {r[len(r)//2]:.2f} p95 {r[int(len(r)*.95)]:.2f} ms")
chunk = struct.pack("<B3xI", 99, 1 << 20) + b"\0" * (1 << 20)
t0 = time.monotonic(); sent = 0
while time.monotonic() - t0 < secs:
    s.sendall(chunk); sent += len(chunk)
dt = time.monotonic() - t0
print(f"bulk PC->Go: {sent / dt / 1e6:.1f} MB/s = {sent * 8 / dt / 1e6:.0f} Mbit/s over {dt:.1f} s")
s.close()

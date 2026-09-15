# HaLow throughput soak: associate, then cycle TCP/UDP up+down until the budget runs out.
# Buffers are fixed size -- nothing here is sized by anything off the wire.
import network
import socket
import time

import halow_config as cfg

HOST = "192.168.0.137"
TCP_PORT = 9001
UDP_PORT = 9002
UDP_DOWN_PORT = 9003
DUR = 5  # seconds per direction
TOTAL_S = 1200  # 20 minutes

buf = bytearray(1460)
mv = memoryview(buf)
udp_mv = memoryview(buf)[:1200]


def kbit(nbytes, ms):
    return (nbytes * 8) // ms if ms else 0


def tcp_run(direction):
    s = socket.socket()
    s.settimeout(20)
    s.connect(socket.getaddrinfo(HOST, TCP_PORT)[0][-1])
    s.send(b"UP\n" if direction == "up" else b"DOWN\n")
    n = 0
    t = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t) < DUR * 1000:
            if direction == "up":
                n += s.send(mv)
            else:
                r = s.readinto(mv)
                if not r:
                    break
                n += r
    except OSError:
        pass
    ms = time.ticks_diff(time.ticks_ms(), t)
    s.close()
    return n, ms


def udp_run(direction):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    port = UDP_PORT if direction == "up" else UDP_DOWN_PORT
    a = socket.getaddrinfo(HOST, port)[0][-1]
    n = 0
    t = time.ticks_ms()
    try:
        if direction == "up":
            while time.ticks_diff(time.ticks_ms(), t) < DUR * 1000:
                n += s.sendto(udp_mv, a)
            s.sendto(b"END", a)
            time.sleep_ms(400)  # let the sink drain before the next phase
        else:
            s.sendto(b"DOWN", a)
            while time.ticks_diff(time.ticks_ms(), t) < DUR * 1000:
                try:
                    r = s.recvfrom(1500)
                except OSError:
                    break
                n += len(r[0])
    except OSError:
        pass
    ms = time.ticks_diff(time.ticks_ms(), t)
    s.close()
    return n, ms


network.country(cfg.COUNTRY)
w = network.HALOW()
w.config(pm=network.HALOW.PM_NONE)
w.active(True)

t0 = time.ticks_ms()
w.connect(cfg.SSID, cfg.KEY)
while not w.isconnected():
    if time.ticks_diff(time.ticks_ms(), t0) > 30000:
        print("RESULT assoc FAILED")
        raise SystemExit
    time.sleep_ms(100)
assoc = time.ticks_diff(time.ticks_ms(), t0)
print("assoc_ms", assoc)
print("ifconfig", w.ifconfig())
print("rssi", w.status("rssi"), "channel", w.config("channel"), "bw", w.config("bandwidth"))

acc = {}
cycles = 0
start = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), start) < TOTAL_S * 1000:
    cycles += 1
    row = []
    for kind, fn in (("tcp", tcp_run), ("udp", udp_run)):
        for d in ("up", "down"):
            n, ms = fn(d)
            k = "%s_%s" % (kind, d)
            r = kbit(n, ms)
            row.append("%s=%d" % (k, r))
            a = acc.setdefault(k, [0, 0, 0, 10**9])  # sum, count, max, min
            a[0] += r
            a[1] += 1
            if r > a[2]:
                a[2] = r
            if r < a[3]:
                a[3] = r
    print("cycle %d %s rssi=%s" % (cycles, " ".join(row), w.status("rssi")))

print("---- SUMMARY (kbit/s) ----")
print("assoc_ms", assoc, "cycles", cycles, "connected", w.isconnected())
for k in sorted(acc):
    s, c, mx, mn = acc[k]
    print("RESULT %-9s mean=%d min=%d max=%d n=%d" % (k, s // c, mn, mx, c))
w.disconnect()
w.active(False)
print("RESULT done")

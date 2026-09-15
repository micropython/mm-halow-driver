# Drop-tolerant HaLow throughput: writes cumulative results to /flash/hres.txt
# after every cycle, so a mid-run USB/CDC drop still leaves the numbers on the device.
import network, socket, time
import halow_config as cfg

HOST = "192.168.0.137"
TCP_PORT = 9001
UDP_PORT = 9002
UDP_DOWN_PORT = 9003
DUR = 5
TOTAL_S = 90
buf = bytearray(1460)
mv = memoryview(buf)
udp_mv = memoryview(buf)[:1200]


def kbit(n, ms):
    return (n * 8) // ms if ms else 0


def tcp_run(d):
    s = socket.socket()
    s.settimeout(20)
    s.connect(socket.getaddrinfo(HOST, TCP_PORT)[0][-1])
    s.send(b"UP\n" if d == "up" else b"DOWN\n")
    n = 0
    t = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t) < DUR * 1000:
            if d == "up":
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


def udp_run(d):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    port = UDP_PORT if d == "up" else UDP_DOWN_PORT
    a = socket.getaddrinfo(HOST, port)[0][-1]
    n = 0
    t = time.ticks_ms()
    try:
        if d == "up":
            while time.ticks_diff(time.ticks_ms(), t) < DUR * 1000:
                n += s.sendto(udp_mv, a)
            s.sendto(b"END", a)
            time.sleep_ms(400)
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


def write(acc, cyc, assoc, conn, rssi):
    try:
        f = open("/flash/hres.txt", "w")
        f.write("assoc_ms %d cycles %d connected %s rssi %s\n" % (assoc, cyc, conn, rssi))
        for k in sorted(acc):
            sm, c, mx, mn = acc[k]
            f.write("RESULT %-9s mean=%d min=%d max=%d n=%d\n" % (k, sm // c, mn, mx, c))
        f.close()
    except Exception as e:
        print("write err", e)


network.country(cfg.COUNTRY)
w = network.HALOW()
w.config(pm=network.HALOW.PM_NONE)
w.active(True)
t0 = time.ticks_ms()
w.connect(cfg.SSID, cfg.KEY)
while not w.isconnected():
    if time.ticks_diff(time.ticks_ms(), t0) > 30000:
        open("/flash/hres.txt", "w").write("assoc FAILED\n")
        raise SystemExit
    time.sleep_ms(100)
assoc = time.ticks_diff(time.ticks_ms(), t0)
print(
    "assoc_ms",
    assoc,
    "rssi",
    w.status("rssi"),
    "channel",
    w.config("channel"),
    "bw",
    w.config("bandwidth"),
)
acc = {}
cyc = 0
start = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), start) < TOTAL_S * 1000:
    cyc += 1
    for kind, fn in (("tcp", tcp_run), ("udp", udp_run)):
        for d in ("up", "down"):
            n, ms = fn(d)
            k = "%s_%s" % (kind, d)
            r = kbit(n, ms)
            a = acc.setdefault(k, [0, 0, 0, 10**9])
            a[0] += r
            a[1] += 1
            if r > a[2]:
                a[2] = r
            if r < a[3]:
                a[3] = r
    print("cycle", cyc, "rssi", w.status("rssi"))
    write(acc, cyc, assoc, w.isconnected(), w.status("rssi"))
write(acc, cyc, assoc, w.isconnected(), w.status("rssi"))
print("DONE")
w.disconnect()
w.active(False)

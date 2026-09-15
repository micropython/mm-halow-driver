"""Throughput peer for the HaLow boards. TCP 9001, UDP 9002.

TCP: client connects and sends "UP\n" (it streams, we sink) or "DOWN\n" (we stream).
UDP: client sends b"UP" datagrams (we sink) or one b"DOWN" (we stream back to its address).
"""

import socket
import threading
import time

BUF = bytes(1460)
DUR_CAP = 30  # never stream longer than this per request


def tcp_client(c):
    try:
        c.settimeout(DUR_CAP + 10)
        head = c.recv(16)
        if head.startswith(b"UP"):
            n = 0
            while True:
                d = c.recv(65536)
                if not d:
                    break
                n += len(d)
            print("tcp up   sank %d bytes" % n, flush=True)
        elif head.startswith(b"DOWN"):
            t = time.time()
            n = 0
            while time.time() - t < DUR_CAP:
                try:
                    c.sendall(BUF)
                    n += len(BUF)
                except OSError:
                    break
            print("tcp down sent %d bytes" % n, flush=True)
    except Exception as e:
        print("tcp err", e, flush=True)
    finally:
        try:
            c.close()
        except Exception:
            pass


def tcp_server():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 9001))
    s.listen(4)
    while True:
        c, _ = s.accept()
        threading.Thread(target=tcp_client, args=(c,), daemon=True).start()


def udp_down_server():
    """Own port: the 9002 sink can be seconds behind draining an up-flood."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 9003))
    while True:
        d, addr = s.recvfrom(2048)
        if d[:4] != b"DOWN":
            continue
        # Paced: blasting from gigabit into a ~2Mbit link just overruns the AP queue
        # and the station receives almost nothing.  Offer a fixed rate instead.
        rate_bps = 4_000_000
        gap = (1200 * 8) / rate_bps
        t = time.time()
        n = 0
        nxt = t
        while time.time() - t < 6:
            try:
                s.sendto(BUF[:1200], addr)
                n += 1200
            except OSError:
                break
            nxt += gap
            d = nxt - time.time()
            if d > 0:
                time.sleep(d)
        print("udp down offered %d bytes (%.1f kbit/s)" % (n, n * 8 / 1000 / 6), flush=True)


def udp_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 9002))
    sank = 0
    while True:
        d, addr = s.recvfrom(2048)
        if d[:4] == b"DOWN":
            t = time.time()
            n = 0
            while time.time() - t < 6:
                try:
                    s.sendto(BUF[:1200], addr)
                    n += 1200
                except OSError:
                    break
            print("udp down sent %d bytes" % n, flush=True)
        elif d[:3] == b"END":
            print("udp up   sank %d bytes" % sank, flush=True)
            sank = 0
        else:
            sank += len(d)


threading.Thread(target=tcp_server, daemon=True).start()
threading.Thread(target=udp_server, daemon=True).start()
threading.Thread(target=udp_down_server, daemon=True).start()
print("thru server up: tcp 9001, udp-up 9002, udp-down 9003", flush=True)
while True:
    time.sleep(3600)

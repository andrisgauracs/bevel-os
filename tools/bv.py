#!/usr/bin/env python3
"""Bevel OS serial tool.

The board's COM port must be held open by one process (opening it can reset the board), so a
small daemon owns the port, logs everything to build/serial.log, and serves commands on
localhost.

  bv.py daemon                 hold the port (run in the background)
  bv.py cmd "<line>" [secs]    send a command line, print the reply up to @@OK / @@ERR
  bv.py dump out.png [nocursor] save the framebuffer as PNG (pixel exact)
  bv.py put local remote      copy a file to the drive (remote like /disk/Apps/x.wad)
  bv.py reset                  reset the board (EN pulse) and print the boot log
  bv.py log [lines]            tail of the serial log
  bv.py quit                   stop the daemon (needed before flashing)
"""
import glob, os, sys, socket, struct, threading, time, zlib

# Serial port of the board's COM (UART) USB port: set BV_PORT, or the first USB serial device is used.
# BV_PORT=tcp:HOST:PORT talks to the desktop simulator (host/, port 47812) or to QEMU
# (tools/qemu.sh, port 5555) instead; "reset" then sends the reboot command.
PORT = os.environ.get("BV_PORT") or next(iter(sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))), "/dev/ttyUSB0")
BAUD = int(os.environ.get("BV_BAUD", "2000000"))
ADDR = ("127.0.0.1", 47811)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "serial.log")
ENDS = (b"@@OK", b"@@ERR", b"@@END")


class TcpSerial:
    """The part of pyserial's Serial the daemon uses, over a TCP connection (reconnects)."""
    def __init__(self, url):
        host, port = url[4:].rsplit(":", 1)
        self.addr = (host, int(port)); self.sock = None; self._rts = False

    def _conn(self):
        while self.sock is None:
            try:
                self.sock = socket.create_connection(self.addr, timeout=2); self.sock.settimeout(None)
            except OSError:
                time.sleep(0.5)
        return self.sock

    def read(self, n):
        # blocking socket, polled: a send to a slow emulator must be able to wait
        import select
        try:
            s = self._conn()
            if not select.select([s], [], [], 0.02)[0]:
                return b""
            d = s.recv(n)
            if not d: self.sock = None
            return d
        except OSError:
            self.sock = None; return b""

    def write(self, data):
        try: self._conn().sendall(data)
        except OSError: self.sock = None

    @property
    def rts(self): return self._rts

    @rts.setter
    def rts(self, v):
        if v and not self._rts: self.write(b"reboot\n")
        self._rts = v


def daemon():
    if PORT.startswith("tcp:"):
        s = TcpSerial(PORT)
    else:
        import serial
        s = serial.Serial()
        s.port = PORT; s.baudrate = BAUD; s.timeout = 0.02
        s.dtr = False; s.rts = False
        s.open()
    lock = threading.Lock()
    listeners = []
    logf = open(LOG, "ab", buffering=0)

    def reader():
        while True:
            try:
                d = s.read(65536)
            except Exception as e:
                logf.write(("\n[daemon] serial error: %s\n" % e).encode()); os._exit(1)
            if d:
                logf.write(d)
                with lock:
                    for q in listeners: q.append(d)
    threading.Thread(target=reader, daemon=True).start()

    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(ADDR); srv.listen(4)
    while True:
        c, _ = srv.accept()
        try:
            req = b""
            while not req.endswith(b"\n"):
                d = c.recv(4096)
                if not d: break
                req += d
            timeout, _, line = req.decode().rstrip("\n").partition("\t")
            timeout = float(timeout)
            if line == "~quit":
                c.sendall(b"bye\n"); c.close(); os._exit(0)
            q = []
            with lock: listeners.append(q)
            if line == "~reset":
                s.rts = True; time.sleep(0.1); s.rts = False
            elif line != "~listen":
                s.write(line.encode() + b"\n")
            t0 = time.time(); buf = b""; done = False
            while time.time() - t0 < timeout and not done:
                with lock:
                    chunk = b"".join(q); q.clear()
                if chunk:
                    buf += chunk
                    c.sendall(chunk)
                    if line[0] != "~":
                        for ln in buf[-4096:].split(b"\n")[:-1]:
                            if ln.startswith(ENDS): done = True
                else:
                    time.sleep(0.005)
            with lock: listeners.remove(q)
        except Exception as e:
            logf.write(("\n[daemon] client error: %r\n" % e).encode())
        finally:
            try: c.close()
            except Exception: pass


def request(line, timeout=10.0):
    c = socket.create_connection(ADDR, timeout=5)
    c.sendall(("%g\t%s\n" % (timeout, line)).encode())
    c.settimeout(timeout + 2)
    out = b""
    while True:
        try:
            d = c.recv(65536)
        except socket.timeout:
            break
        if not d: break
        out += d
    return out


def write_png(path, w, h, rgb, scale=1):
    if scale > 1:
        rows = []
        for y in range(h):
            row = rgb[y * w * 3:(y + 1) * w * 3]
            big = b"".join(row[i:i + 3] * scale for i in range(0, len(row), 3))
            rows.extend([big] * scale)
        w *= scale; h *= scale; rgb = b"".join(rows)
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def dump(path, extra="", crop=None, scale=1):
    out = request(("dump " + extra).strip(), 30)
    i = out.find(b"@@FB ")
    if i < 0: sys.exit("no framebuffer header in reply: %r" % out[:200])
    j = out.index(b"\n", i)
    w, h, n = map(int, out[i + 5:j].split())
    data = out[j + 1:j + 1 + n]
    if len(data) < n: sys.exit("short dump: %d of %d bytes" % (len(data), n))
    lv = (0, 85, 170, 255)
    pal = [bytes((lv[p & 3], lv[(p >> 2) & 3], lv[(p >> 4) & 3])) for p in range(256)]
    rgb = bytearray()
    for k in range(0, n, 2):
        rgb += pal[data[k + 1]] * data[k]
    if len(rgb) != w * h * 3: sys.exit("bad RLE length %d" % len(rgb))
    if crop:
        x, y, cw, ch = crop
        rgb = b"".join(bytes(rgb[((y + r) * w + x) * 3:((y + r) * w + x + cw) * 3]) for r in range(ch))
        w, h = cw, ch
    write_png(path, w, h, bytes(rgb), scale)
    print("saved", path, w, h)


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    if a[0] == "daemon": daemon()
    elif a[0] == "cmd":
        sys.stdout.write(request(a[1], float(a[2]) if len(a) > 2 else 10).decode(errors="replace"))
    elif a[0] == "dump":
        extra = "nocursor" if "nocursor" in a else ""
        crop = None; scale = 1
        for x in a[2:]:
            if x.startswith("crop="): crop = tuple(map(int, x[5:].split(",")))
            if x.startswith("scale="): scale = int(x[6:])
        dump(a[1], extra, crop, scale)
    elif a[0] == "put":
        import base64
        data = open(a[1], "rb").read()
        if b"@@OK" not in request("putopen " + a[2], 10): sys.exit("cannot create " + a[2])
        t0 = time.time()
        for i in range(0, len(data), 3072):
            # each chunk names its offset, so it can be sent again until it is acknowledged
            chunk = data[i:i + 3072]
            line = "putat %d %s" % (i, base64.b64encode(chunk).decode())
            ack = ("@@AT %d %d\n" % (i, zlib.adler32(chunk))).encode()
            for attempt in range(8):
                if ack in request(line, 8): break
                print("(retry at %d)" % i, end=" ", flush=True)
            else:
                sys.exit("write failed at %d" % i)
            if (i // 3072) % 100 == 0: print("%d%%" % (i * 100 // len(data)), end=" ", flush=True)
        if b"@@OK" not in request("putclose", 20): sys.exit("close failed")
        size = request("size " + a[2], 10).decode(errors="replace")
        if str(len(data)) not in size.split(): sys.exit("\nsize check failed: %s" % size.strip())
        total = request("sum " + a[2], 120).decode(errors="replace")
        if str(zlib.adler32(data)) not in total.split(): sys.exit("\ncontent check failed: %s" % total.strip())
        print("\nsent %d bytes in %.0f s, size and checksum verified" % (len(data), time.time() - t0))
    elif a[0] == "reset":
        sys.stdout.write(request("~reset", float(a[1]) if len(a) > 1 else 4).decode(errors="replace"))
    elif a[0] == "listen":
        sys.stdout.write(request("~listen", float(a[1]) if len(a) > 1 else 3).decode(errors="replace"))
    elif a[0] == "log":
        n = int(a[1]) if len(a) > 1 else 40
        with open(LOG, "rb") as f: print(b"\n".join(f.read().split(b"\n")[-n:]).decode(errors="replace"))
    elif a[0] == "quit":
        try: print(request("~quit", 2).decode())
        except OSError: print("daemon not running")
    else:
        sys.exit(__doc__)

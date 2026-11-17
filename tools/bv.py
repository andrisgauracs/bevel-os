#!/usr/bin/env python3
"""Bevel OS serial tool.

The board's USB-Serial-JTAG port ("USB1") carries the log and the command channel. Only one
process can have it open, so a small daemon owns the port, logs everything to build/serial.log,
and serves commands on localhost.

  bv.py daemon                 hold the port (run in the background)
  bv.py cmd "<line>" [secs]    send a command line, print the reply up to @@OK / @@ERR
  bv.py dump out.png [nocursor] save the framebuffer as PNG (pixel exact)
  bv.py put local remote      copy a file to the drive (remote like /disk/Apps/x.wad)
  bv.py reset                  restart the board ("reboot" command) and print the boot log
  bv.py log [lines]            tail of the serial log
  bv.py quit                   stop the daemon (needed before flashing)

  bv.py direct "<line>" [secs]    send a command line without the daemon: opens the port itself and
                                  prints what came back. Use it when a background process or a
                                  listening socket is not allowed (a sandboxed session, say).
  bv.py directdump out.png [nocursor]   save the framebuffer as PNG, also without the daemon
"""
import glob, os, sys, socket, struct, threading, time, zlib

def find_port():
    """BV_PORT, else the first Espressif USB-Serial-JTAG device (303A:1001), else a likely default."""
    if os.environ.get("BV_PORT"): return os.environ["BV_PORT"]
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            if p.vid == 0x303A and p.pid == 0x1001: return p.device
    except Exception:
        pass
    return next(iter(sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))), "COM13" if os.name == "nt" else "/dev/ttyACM0")


# The S3 console is UART0 at 460800 baud behind a USB-UART bridge (its CH340 lost bytes at 2 Mbaud);
# the P4 console is USB-Serial-JTAG, which ignores the rate. BV_BAUD overrides.
BAUD = int(os.environ.get("BV_BAUD", "460800"))
ADDR = ("127.0.0.1", 47811)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "serial.log")
ENDS = (b"@@OK", b"@@ERR", b"@@END")


def big_rx(s):
    """Windows gives a serial port a 4 KB receive buffer, which a short pause on this side can
    overflow during a dump. Ask for 1 MB (other systems ignore this)."""
    try:
        s.set_buffer_size(rx_size=1 << 20, tx_size=1 << 16)
    except (AttributeError, Exception):
        pass


def open_port():
    """The board's console port, held by us instead of by the daemon."""
    import serial
    s = serial.Serial()
    s.port = find_port(); s.baudrate = BAUD; s.timeout = 0.02; s.write_timeout = 5
    s.dtr = False; s.rts = False
    s.open()
    big_rx(s)
    return s


def log_write(data):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, "ab", buffering=0) as f:
        f.write(data)


def drain(s, secs=0.3):
    """Throw away whatever the board logged before our first command."""
    t0 = time.time()
    while time.time() - t0 < secs:
        s.read(4096)


def direct(lines, secs=8.0):
    """Send command lines without the daemon: open the port, send, read until @@OK / @@ERR."""
    s = open_port()
    out = b""
    try:
        drain(s)
        for ln in lines:
            s.write(ln.encode() + b"\n")
            end, got = time.time() + secs, b""
            while time.time() < end:
                d = s.read(4096)
                if not d:
                    continue
                got += d
                if not ln.startswith("~") and any(m in got for m in ENDS):
                    break
            out += got
    finally:
        s.close()
    log_write(out)
    return out


def direct_dump(path, extra="", crop=None, scale=1, secs=60.0):
    """The framebuffer as PNG, also without the daemon (the payload is longer than a command reply)."""
    if use_bands():
        s = open_port()
        try:
            drain(s)
            return banded(lambda line: direct_reply(s, line), path, extra, crop, scale)
        finally:
            s.close()
    s = open_port()
    out, need = b"", None
    try:
        drain(s)
        s.write(("dump " + extra).strip().encode() + b"\n")
        deadline = time.time() + secs
        while time.time() < deadline:
            out += s.read(65536)
            if need is None:
                i = out.find(b"@@FB16 ")
                if i >= 0 and b"\n" in out[i:]:
                    j = out.index(b"\n", i)
                    need = j + 1 + int(out[i + 7:j].split()[2])
            elif len(out) >= need:
                break
    finally:
        s.close()
    log_write(out)
    if need is not None and len(out) < need:
        print("dump arrived %d of %d bytes: the rows missing from the end will be black" % (len(out), need))
    decode_dump(out, path, crop, scale, pad=True)


def direct_put(local, remote, chunk=3072):
    """Copy a file to the drive without the daemon, same putopen/putdata/putclose protocol."""
    import base64
    data = open(local, "rb").read()
    s, t0 = open_port(), time.time()

    def send(line, secs=30):
        s.write(line.encode() + b"\n")
        end, got = time.time() + secs, b""
        while time.time() < end:
            d = s.read(4096)
            if d:
                got += d
                if any(m in got for m in ENDS):
                    break
        if b"@@OK" not in got:
            raise SystemExit("%s failed: %r" % (line.split()[0], got[-150:]))

    try:
        drain(s)
        send("putopen " + remote)
        for i in range(0, len(data), chunk):
            send("putdata " + base64.b64encode(data[i:i + chunk]).decode())
            if (i // chunk) % 100 == 0:
                print("%d%%" % (i * 100 // len(data)), end=" ", flush=True)
        send("putclose", 120)
    finally:
        s.close()
    print("\nsent %d bytes in %.0f s" % (len(data), time.time() - t0))


def daemon():
    import serial
    s = serial.Serial()
    s.port = find_port(); s.baudrate = BAUD; s.timeout = 0.02; s.write_timeout = 5
    s.dtr = False; s.rts = False
    s.open()
    big_rx(s)
    lock = threading.Lock()
    listeners = []
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
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
                s.write(b"reboot\n")     # an RTS pulse does not reset this board; the port survives the restart
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


BAND = 32   # rows per piece of a banded dump


def use_bands():
    """A USB-UART bridge (the S3's CH340) can lose the odd byte, so over anything but the P4's
    USB-Serial-JTAG port dumps come in bands of rows, each checked and asked for again if it is short
    or its sum is wrong. BV_BANDS=0/1 overrides."""
    if os.environ.get("BV_BANDS") is not None: return os.environ["BV_BANDS"] == "1"
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            if p.device == find_port(): return not (p.vid == 0x303A and p.pid == 0x1001)
    except Exception:
        pass
    return True


def parse_band(out):
    """(w, h, y0, rows, payload) of one band reply, or None if it did not arrive whole."""
    i = out.find(b"@@FBB ")
    if i < 0 or b"\n" not in out[i:]: return None
    j = out.index(b"\n", i)
    try:
        w, h, y0, rows, n, total = map(int, out[i + 6:j].split())
    except ValueError:
        return None
    data = out[j + 1:j + 1 + n]
    if len(data) != n or sum(data) & 0xFFFFFFFF != total: return None
    if sum(data[k] for k in range(0, n, 3)) != w * rows: return None
    return w, h, y0, rows, data


def banded(fetch, path, extra="", crop=None, scale=1):
    """The screen band by band; fetch(line) returns one reply. The link loses an odd byte now and
    then, so a band that does not add up is asked for again at half the height (down to one row), and
    the height grows back after each band that arrives whole: a quiet desktop goes in large bands, a
    noisy picture (Doom) in small ones."""
    y, h, w, parts, size, fails = 0, None, 0, [], BAND, 0
    while h is None or y < h:
        got = parse_band(fetch(("dump %s band %d %d" % (extra, y, size)).replace("  ", " ")))
        if not got:
            fails += 1
            if fails > 40: sys.exit("rows %d.. never arrived whole" % y)
            size = max(1, size // 2)
            continue
        w, h, y0, rows, data = got
        parts.append(data)
        y += rows
        size = min(BAND, size * 2)
    payload = b"".join(parts)
    decode_dump(b"@@FB16 %d %d %d\n" % (w, h, len(payload)) + payload, path, crop, scale)


def dump(path, extra="", crop=None, scale=1):
    if use_bands(): return banded(lambda line: request(line, 20), path, extra, crop, scale)
    decode_dump(request(("dump " + extra).strip(), 60), path, crop, scale)


def direct_reply(s, line, secs=20.0):
    """One command over an open port: the bytes up to and including @@END / @@OK / @@ERR."""
    drain(s, 0.05)
    s.write(line.encode() + b"\n")
    out, deadline = b"", time.time() + secs
    while time.time() < deadline:
        out += s.read(65536)
        if any(e + b"\n" in out for e in ENDS): break
    log_write(out)
    return out


def decode_dump(out, path, crop=None, scale=1, pad=False):
    i = out.find(b"@@FB16 ")
    if i < 0: sys.exit("no framebuffer header in reply: %r" % out[:200])
    j = out.index(b"\n", i)
    w, h, n = map(int, out[i + 7:j].split())
    data = out[j + 1:j + 1 + n]
    if len(data) < n:
        # a busy program (Doom runs at the same priority as the serial task) can overflow the
        # console's TX ring in the middle of a dump: fill the gap with zero counts, which draw black
        if not pad: sys.exit("short dump: %d of %d bytes" % (len(data), n))
        data += b"\x00" * (n - len(data))
    # (count, RGB565 low byte, high byte) triplets: what the panel shows, overlays and pointer included
    rgb = bytearray()
    cache = {}
    for k in range(0, n, 3):
        v = data[k + 1] | data[k + 2] << 8
        px = cache.get(v)
        if px is None:
            r, g, b = v >> 11, (v >> 5) & 63, v & 31
            px = cache[v] = bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31))
        rgb += px * data[k]
    if len(rgb) != w * h * 3:
        if not pad: sys.exit("bad RLE length %d" % len(rgb))
        if len(rgb) < w * h * 3: rgb += b"\0" * (w * h * 3 - len(rgb))
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
    elif a[0] in ("direct", "directdump"):
        args = list(a[1:])
        secs = 60.0 if a[0] == "directdump" else 8.0
        try:
            secs = float(args[-1]); args = args[:-1]
        except (IndexError, ValueError):
            pass
        if a[0] == "direct":
            sys.stdout.write(direct(args, secs).decode(errors="replace"))
        else:
            direct_dump(args[0], "nocursor" if "nocursor" in args[1:] else "", secs=secs)
    elif a[0] == "directput":
        direct_put(a[1], a[2])
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
            if b"@@OK" not in request("putdata " + base64.b64encode(data[i:i + 3072]).decode(), 20): sys.exit("write failed at %d" % i)
            if (i // 3072) % 100 == 0: print("%d%%" % (i * 100 // len(data)), end=" ", flush=True)
        if b"@@OK" not in request("putclose", 20): sys.exit("close failed")
        print("\nsent %d bytes in %.0f s" % (len(data), time.time() - t0))
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

#!/usr/bin/env python3
"""VNC server acceptance test for Bevel OS.

Connects to the VNC server as a viewer (a small RFB client written here, no dependencies),
checks the picture it receives pixel by pixel against the framebuffer read over the serial
channel, and drives the OS with VNC keyboard and mouse events.

Works against the board, QEMU or the desktop simulator. Needs the serial daemon
(tools/bv.py daemon) for the reference framebuffer and the window list.

  vnctest.py [--host H] [--port P] [--password PW]

The password check uses the openssl command for DES, independently of the server's code.
"""
import os, socket, struct, subprocess, sys, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bv

def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

HOST = arg("--host", "127.0.0.1")
PORT = int(arg("--port", "5900"))
PASSWORD = arg("--password", "")
LEVEL = (0, 85, 170, 255)
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok or not detail else "   (%s)" % detail), flush=True)
    return ok


# ---------------------------------------------------------------- reference picture (serial)

def reference(cursor=False):
    out = bv.request("dump" + ("" if cursor else " nocursor"), 30)
    i = out.find(b"@@FB ")
    j = out.index(b"\n", i)
    w, h, n = map(int, out[i + 5:j].split())
    data = out[j + 1:j + 1 + n]
    px = bytearray()
    for k in range(0, n, 2):
        px += bytes([data[k + 1]]) * data[k]
    return w, h, bytes(px)


def wins():
    return bv.request("wins", 10).decode(errors="replace")


def close_all():
    """Closes every window (Alt+F4 on the desktop would offer to shut down: Esc that)."""
    for _ in range(8):
        w = wins()
        if "win " not in w:
            return
        if 'win "Shut Down' in w or 'win "Save' in w or "Program" in w:
            bv.request("key esc", 5)
        else:
            bv.request("key alt+f4", 5)
        time.sleep(0.1)


# ---------------------------------------------------------------- RFB client

def des_ecb(key, block):
    r = subprocess.run(["openssl", "enc", "-des-ecb", "-K", key.hex(), "-nopad", "-provider", "legacy",
                        "-provider", "default"], input=block, capture_output=True, check=True)
    return r.stdout


def vnc_response(password, challenge):
    key = bytes(int("{:08b}".format(b)[::-1], 2) for b in password.encode()[:8].ljust(8, b"\0"))
    return des_ecb(key, challenge[:8]) + des_ecb(key, challenge[8:])


class Viewer:
    def __init__(self, host, port, password="", version=b"RFB 003.008\n"):
        self.s = socket.create_connection((host, port), timeout=10)
        self.buf = b""
        server_version = self.read(12)
        assert server_version.startswith(b"RFB 003."), server_version
        self.s.sendall(version)
        minor = int(version[8:11])
        if minor == 3:
            sec = struct.unpack(">I", self.read(4))[0]
        else:
            n = self.read(1)[0]
            types = self.read(n)
            sec = 2 if 2 in types else types[0]
            self.s.sendall(bytes([sec]))
        self.security = sec
        if sec == 2:
            challenge = self.read(16)
            self.s.sendall(vnc_response(password, challenge))
            ok = struct.unpack(">I", self.read(4))[0]
            if ok != 0:
                raise PermissionError("authentication failed")
        elif minor == 8:
            assert struct.unpack(">I", self.read(4))[0] == 0
        self.s.sendall(b"\x01")   # shared
        self.w, self.h = struct.unpack(">HH", self.read(4))
        self.server_pf = self.read(16)
        nlen = struct.unpack(">I", self.read(4))[0]
        self.name = self.read(nlen).decode()
        self.fb = bytearray(self.w * self.h)   # kept as RGB222 indexes, whatever the wire format
        self.bpp = 1
        self.decode = None
        self.cursor = None
        self.pointer = None
        self.set_format8()

    def read(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise ConnectionError("closed by server")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def set_format8(self):
        self.bpp = 1
        self.decode = lambda b: b[0]
        self.s.sendall(b"\x00\0\0\0" + bytes([8, 6, 0, 1, 0, 3, 0, 3, 0, 3, 0, 2, 4, 0, 0, 0]))

    def set_format32(self):
        # 32 bpp little endian, 8 bits per channel: what noVNC and most viewers ask for
        self.bpp = 4
        lv = {v: i for i, v in enumerate(LEVEL)}
        def dec(b):
            v = b[0] | b[1] << 8 | b[2] << 16
            r, g, bl = (v >> 16) & 255, (v >> 8) & 255, v & 255
            return lv[r] | lv[g] << 2 | lv[bl] << 4
        self.decode = dec
        self.s.sendall(b"\x00\0\0\0" + struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))

    def set_encodings(self, encs):
        self.s.sendall(struct.pack(">BxH", 2, len(encs)) + b"".join(struct.pack(">i", e) for e in encs))

    def request(self, incremental, x=0, y=0, w=None, h=None):
        self.s.sendall(struct.pack(">BBHHHH", 3, incremental, x, y, w or self.w, h or self.h))

    def key(self, keysym, down):
        self.s.sendall(struct.pack(">BBxxI", 4, down, keysym))

    def tap(self, keysym):
        self.key(keysym, 1); self.key(keysym, 0)

    def pointer_event(self, x, y, mask):
        self.s.sendall(struct.pack(">BBHH", 5, mask, x, y))

    def pixels(self, n):
        raw = self.read(n * self.bpp)
        if self.bpp == 1:
            return raw
        return bytes(self.decode(raw[i:i + 4]) for i in range(0, len(raw), 4))

    def update(self, timeout=5.0):
        """Reads one FramebufferUpdate; returns the list of (x, y, w, h, encoding)."""
        self.s.settimeout(timeout)
        while True:
            t = self.read(1)[0]
            if t == 0:
                break
            if t == 2:   # bell
                continue
            if t == 3:
                self.read(3); n = struct.unpack(">I", self.read(4))[0]; self.read(n); continue
            raise ValueError("unexpected server message %d" % t)
        self.read(1)
        n = struct.unpack(">H", self.read(2))[0]
        rects = []
        for _ in range(n):
            x, y, w, h, enc = struct.unpack(">HHHHi", self.read(12))
            rects.append((x, y, w, h, enc))
            if enc == 0:
                data = self.pixels(w * h)
                for r in range(h):
                    self.fb[(y + r) * self.w + x:(y + r) * self.w + x + w] = data[r * w:(r + 1) * w]
            elif enc == 5:
                self.hextile(x, y, w, h)
            elif enc == -239:
                px = self.pixels(w * h)
                mask = self.read(((w + 7) // 8) * h)
                self.cursor = (x, y, w, h, px, mask)
            elif enc == -232:
                self.pointer = (x, y)
            else:
                raise ValueError("unexpected encoding %d" % enc)
        self.s.settimeout(10)
        return rects

    def hextile(self, rx, ry, rw, rh):
        bg = fg = None
        for ty in range(ry, ry + rh, 16):
            th = min(16, ry + rh - ty)
            for tx in range(rx, rx + rw, 16):
                tw = min(16, rx + rw - tx)
                flags = self.read(1)[0]
                if flags & 1:
                    data = self.pixels(tw * th)
                    for r in range(th):
                        self.fb[(ty + r) * self.w + tx:(ty + r) * self.w + tx + tw] = data[r * tw:(r + 1) * tw]
                    continue
                if flags & 2: bg = self.pixels(1)[0]
                if flags & 4: fg = self.pixels(1)[0]
                assert bg is not None, "background colour never sent"
                for r in range(th):
                    self.fb[(ty + r) * self.w + tx:(ty + r) * self.w + tx + tw] = bytes([bg]) * tw
                if flags & 8:
                    n = self.read(1)[0]
                    for _ in range(n):
                        c = self.pixels(1)[0] if flags & 16 else fg
                        xy, wh = self.read(2)
                        sx, sy, sw, sh = xy >> 4, xy & 15, (wh >> 4) + 1, (wh & 15) + 1
                        for r in range(sh):
                            o = (ty + sy + r) * self.w + tx + sx
                            self.fb[o:o + sw] = bytes([c]) * sw

    def settle(self, quiet=0.6, limit=8.0):
        """Keeps asking for incremental updates until nothing changes for `quiet` seconds."""
        t0 = time.time()
        last = time.time()
        while time.time() - t0 < limit:
            self.request(1)
            try:
                rects = self.update(timeout=quiet)
            except socket.timeout:
                return True
            if any(r[4] >= 0 for r in rects):
                last = time.time()
            elif time.time() - last > quiet:
                return True
        return False


def diff(a, b, w):
    bad = [i for i in range(len(a)) if a[i] != b[i]]
    if not bad:
        return ""
    xs = [i % w for i in bad]; ys = [i // w for i in bad]
    return "%d pixels differ in %d,%d - %d,%d" % (len(bad), min(xs), min(ys), max(xs), max(ys))


def same_as_screen(v, name):
    # the taskbar clock may tick between the viewer's picture and the serial dump: try again
    for _ in range(3):
        v.settle()
        w, h, ref = reference()
        d = diff(v.fb, ref, w)
        if not d:
            break
    return check(name, not d, d)


def wait_for(pattern, timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if re.search(pattern, wins()):
            return True
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------- the test

def main():
    print("VNC server at %s:%d" % (HOST, PORT))
    # wrong password must be refused (only meaningful when a password is set)
    if PASSWORD:
        try:
            Viewer(HOST, PORT, "wrong!")
            check("a wrong password is refused", False)
        except (PermissionError, ConnectionError):
            check("a wrong password is refused", True)
        time.sleep(1.2)

    v = Viewer(HOST, PORT, PASSWORD)
    check("RFB 3.8 handshake%s, server init 640x480 \"%s\"" % (" with VNC authentication" if v.security == 2 else "", v.name),
          (v.w, v.h) == (640, 480) and v.server_pf[:4] == bytes([8, 6, 0, 1]))

    # start from a known state: everything closed
    close_all()
    bv.request("mouse 600 300", 5)

    # 1. raw, the server's own 8-bit format
    v.set_encodings([0])
    v.request(0)
    rects = v.update()
    check("full update, Raw, native RGB222 (%d rectangle)" % len(rects), rects[0][:4] == (0, 0, 640, 480))
    w, h, ref = reference(cursor=True)   # no Cursor encoding: the pointer is in the pixels
    d = diff(v.fb, ref, w)
    check("  picture identical to the framebuffer, pointer included", not d, d)

    # 2. hextile + cursor + pointer position, 32 bpp
    v.set_format32()
    v.set_encodings([5, 0, -239, -232])
    v.request(0)
    v.update()
    v.settle()
    w, h, ref = reference(cursor=False)
    d = diff(v.fb, ref, w)
    check("full update, Hextile, 32 bpp true colour: identical, pointer not in the pixels", not d, d)
    check("  pointer shape sent (Cursor pseudo-encoding)", v.cursor is not None and v.cursor[2] > 0,
          "no cursor rectangle")

    # 3. keyboard: F1 opens Help, then incremental updates follow
    v.tap(0xFFBE)   # F1
    check("F1 from the viewer opens Help", wait_for('win "Help'), wins())
    same_as_screen(v, "  incremental updates match the screen")

    # 4. typing with Shift-dependent characters into the editor
    close_all()
    bv.request("rm /disk/Documents/vnc.txt", 5)
    bv.request("launch Editor", 5)
    if not check("the text editor opens", wait_for("Editor"), wins()):
        pass
    else:
        text = "Hello, VNC! (A+b) #42 ~ok?"
        for ch in text:
            ks = ord(ch)
            shifted = ch in '!@#$%^&*()_+{}|:"<>?~' or ch.isupper()
            # like a real viewer: Shift goes down for shifted characters
            if shifted: v.key(0xFFE1, 1)
            v.tap(ks)
            if shifted: v.key(0xFFE1, 0)
        time.sleep(0.3)
        # an AZERTY-style viewer: '1' typed with Shift held must still give '1'
        v.key(0xFFE1, 1); v.tap(ord("1")); v.key(0xFFE1, 0)
        time.sleep(0.3)
        time.sleep(1.5)   # let the OS take in every key (the emulator is slow)
        v.key(0xFFE3, 1); v.tap(ord("s")); v.key(0xFFE3, 0)   # Ctrl+S: save as
        wait_for('active: Save As')
        for _ in range(16): v.tap(0xFF08)        # clear the proposed name
        for ch in "vnc.txt": v.tap(ord(ch))
        v.tap(0xFF0D)
        wait_for('active: [^|]*Editor', 10)
        saved = bv.request("cat /disk/Documents/vnc.txt", 5).decode(errors="replace")
        check("characters typed through VNC, Shift as each one needs", text + "1" in saved, saved[-120:])
        same_as_screen(v, "  editor picture matches the screen")
        close_all()
        bv.request("rm /disk/Documents/vnc.txt", 5)

    # 5. mouse: a double click on the first desktop icon opens Files
    close_all()
    time.sleep(0.3)
    for _ in range(2):
        v.pointer_event(44, 30, 1); v.pointer_event(44, 30, 0)
    check("double click from the viewer opens Files", wait_for('win "Files'), wins())
    # drag the window by its title bar
    m = re.search(r'win "Files[^"]*" (-?\d+),(-?\d+)', wins())
    if m:
        x0, y0 = int(m.group(1)), int(m.group(2))
        v.pointer_event(x0 + 80, y0 + 10, 0); v.pointer_event(x0 + 80, y0 + 10, 1)
        for k in range(1, 11):
            v.pointer_event(x0 + 80 - 6 * k, y0 + 10 + 4 * k, 1)
            time.sleep(0.02)
        v.pointer_event(x0 + 20, y0 + 50, 0)
        time.sleep(0.4)
        m2 = re.search(r'win "Files[^"]*" (-?\d+),(-?\d+)', wins())
        check("dragging the title bar moves the window", m2 and (int(m2.group(1)), int(m2.group(2))) == (x0 - 60, y0 + 40),
              m2 and m2.group(0))
    same_as_screen(v, "  picture after the drag matches the screen")
    # the pointer position follows server-side moves (PointerPos)
    bv.request("mouse 321 123", 5)
    v.settle()
    check("pointer moved by the OS is reported (PointerPos)", v.pointer == (321, 123), v.pointer)

    # 6. wheel and a second viewer taking over
    close_all()
    v2 = Viewer(HOST, PORT, PASSWORD, version=b"RFB 003.003\n")
    v2.set_encodings([5, 0])
    v2.request(0)
    v2.update()
    v2.settle()
    w, h, ref = reference(cursor=True)
    d = diff(v2.fb, ref, w)
    check("a second viewer (RFB 3.3) takes over and gets the screen", not d, d)
    try:
        v.request(1)
        v.update(timeout=2)
        v.update(timeout=2)
        dropped = False
    except (ConnectionError, OSError):
        dropped = True
    check("  the first viewer is disconnected", dropped)

    status = bv.request("vnc", 5).decode(errors="replace")
    check("serial 'vnc' command reports the viewer", "viewer" in status and "no viewer" not in status, status.strip())

    print("\n%d of %d checks passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()

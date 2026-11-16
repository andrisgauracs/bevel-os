// VNC server (RFB 3.3, 3.7 and 3.8): the desktop over the network.
//
// The framebuffer is RGB222, which RFB can describe exactly (8 bits per pixel, 2 bits per
// channel), so a viewer that keeps the server's format receives the framebuffer bytes as they
// are; any other true-colour format goes through a 64-entry table. Only what changed is sent:
// the compositor reports every rectangle it writes (vnc_damage), and the encoder reads the
// framebuffer afterwards. A rectangle that is being redrawn while it is read is reported again,
// so the viewer always ends up with the final picture without any locking of the framebuffer.
//
// The pointer is a sprite outside the framebuffer: viewers that support the Cursor
// pseudo-encoding draw it themselves (a pointer move costs nothing on the wire); for the others
// it is composited into the pixels that are sent.
//
// Encodings: Raw and Hextile (flat-colour UI compresses well and costs little CPU), plus the
// Cursor and PointerPos pseudo-encodings. One viewer at a time; a new one takes over.
// The same code runs on lwIP (the board, QEMU) and POSIX sockets (the desktop simulator).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "vnc.h"

#if !CONFIG_BEVEL_VNC

void vnc_start(void) {}
void vnc_status(char *buf, int n) { snprintf(buf, n, "VNC: not built in"); }
void vnc_damage(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }

#else

#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "vga.h"
#include "gfx.h"
#include "input.h"
#include "net.h"
#include "os.h"

static const char *TAG = "vnc";

// The simulator listens on the loopback interface only, unless asked otherwise.
#if CONFIG_BEVEL_VNC_LOCALHOST_ONLY
#define VNC_BIND INADDR_LOOPBACK
#else
#define VNC_BIND INADDR_ANY
#endif

#define FB_W VGA_W
#define FB_H VGA_H
#define MAX_DAMAGE 16
#define OUT_BYTES 16384
#define MAX_KEYS 16

enum { ENC_RAW = 0, ENC_HEXTILE = 5, ENC_CURSOR = -239, ENC_POINTERPOS = -232 };
enum { HT_RAW = 1, HT_BG = 2, HT_FG = 4, HT_ANY = 8, HT_COLOURED = 16 };

// ---------------------------------------------------------------- damage (any task)

static SemaphoreHandle_t s_lock;
static rect_t s_dmg[MAX_DAMAGE];
static int s_ndmg;
static volatile bool s_active;          // a viewer is connected: damage is worth recording

static int area(const rect_t *r) { return r->w * r->h; }

static void damage_add(rect_t r)
{
    for (;;) {
        int merged = -1;
        for (int i = 0; i < s_ndmg; i++) {
            rect_t u;
            rect_union(&u, &r, &s_dmg[i]);
            if (area(&u) <= area(&r) + area(&s_dmg[i]) + 1024) { r = u; merged = i; break; }
        }
        if (merged < 0) break;
        s_dmg[merged] = s_dmg[--s_ndmg];
    }
    if (s_ndmg < MAX_DAMAGE) { s_dmg[s_ndmg++] = r; return; }
    int best = 0, cost = INT32_MAX;
    for (int i = 0; i < s_ndmg; i++) {
        rect_t u;
        rect_union(&u, &r, &s_dmg[i]);
        if (area(&u) - area(&s_dmg[i]) < cost) { cost = area(&u) - area(&s_dmg[i]); best = i; }
    }
    rect_union(&s_dmg[best], &s_dmg[best], &r);
}

void vnc_damage(int x, int y, int w, int h)
{
    if (!s_active || !s_lock) return;
    rect_t r = {x, y, w, h}, screen = {0, 0, FB_W, FB_H};
    if (!rect_intersect(&r, &r, &screen)) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    damage_add(r);
    xSemaphoreGive(s_lock);
}

static int damage_take(rect_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_ndmg;
    memcpy(out, s_dmg, n * sizeof(rect_t));
    s_ndmg = 0;
    xSemaphoreGive(s_lock);
    return n;
}

// ---------------------------------------------------------------- viewer state

typedef struct {
    int fd;
    char peer[48];
    // pixel format requested by the viewer
    int bpp;                    // bytes per pixel: 1, 2 or 4
    uint8_t px[64][4];          // each RGB222 colour in the viewer's format, wire byte order
    bool native;                // the viewer kept the server's format: framebuffer bytes go out as they are
    bool hextile, cursor_enc, pointer_enc;
    // update requests
    bool want;                  // a request is outstanding
    bool full;                  // ... and it is not incremental: send the requested area
    rect_t full_area;
    // pointer
    uint32_t cur_serial;        // last shape sent (Cursor encoding)
    int ptr_x, ptr_y;           // where the viewer believes the pointer is
    rect_t sw_cur;              // composited pointer rectangle (no Cursor encoding), w=0 if none
    // keyboard: keys this viewer holds down, so key-up and disconnect release the right usage
    uint32_t key_sym[MAX_KEYS];
    uint8_t key_usage[MAX_KEYS];
    uint8_t buttons;
    // output
    uint8_t *out;
    int outn;
    bool err;
    // hextile carry-over
    int bg, fg;                 // last background/foreground colour sent, -1 = none
} client_t;

static volatile int s_state;      // 0 waiting for network, 1 listening, 2 viewer connected, -1 failed
static char s_peer[48];
static uint8_t *s_band;           // up to 16 rows of the rectangle being encoded (RGB222)
static uint8_t *s_out;            // send buffer
static uint8_t s_cur_img[VGA_CUR_MAX_W * VGA_CUR_MAX_H];

void vnc_status(char *buf, int n)
{
    net_status_t ns;
    net_status(&ns);
    switch (s_state) {
    case 0: snprintf(buf, n, "VNC: waiting for the network"); break;
    case 1: snprintf(buf, n, "VNC: port %d on %s, no viewer", CONFIG_BEVEL_VNC_PORT, VNC_BIND == INADDR_LOOPBACK ? "127.0.0.1" : ns.ip[0] ? ns.ip : "all addresses"); break;
    case 2: snprintf(buf, n, "VNC: %s:%d, viewer %s", ns.ip[0] ? ns.ip : "*", CONFIG_BEVEL_VNC_PORT, s_peer); break;
    default: snprintf(buf, n, "VNC: could not open port %d", CONFIG_BEVEL_VNC_PORT); break;
    }
}

// ---------------------------------------------------------------- socket I/O

static bool rd(client_t *c, void *buf, int n)
{
    uint8_t *p = buf;
    while (n > 0) {
        int r = recv(c->fd, p, n, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; c->err = true; return false; }
        p += r; n -= r;
    }
    return true;
}

static bool skip(client_t *c, uint32_t n)
{
    uint8_t tmp[64];
    while (n > 0) {
        int k = n > sizeof(tmp) ? sizeof(tmp) : (int)n;
        if (!rd(c, tmp, k)) return false;
        n -= k;
    }
    return true;
}

static void flush_out(client_t *c)
{
    int off = 0;
    while (!c->err && off < c->outn) {
        int r = send(c->fd, c->out + off, c->outn - off, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; c->err = true; break; }
        off += r;
    }
    c->outn = 0;
}

static void put(client_t *c, const void *data, int n)
{
    const uint8_t *p = data;
    while (n > 0) {
        if (c->outn == OUT_BYTES) flush_out(c);
        int k = OUT_BYTES - c->outn < n ? OUT_BYTES - c->outn : n;
        memcpy(c->out + c->outn, p, k);
        c->outn += k; p += k; n -= k;
    }
}

static void put8(client_t *c, uint8_t v) { put(c, &v, 1); }
static void put16(client_t *c, uint16_t v) { uint8_t b[2] = {v >> 8, v}; put(c, b, 2); }
static void put32(client_t *c, uint32_t v) { uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v}; put(c, b, 4); }
static void put_px(client_t *c, uint8_t v) { put(c, c->px[v & 63], c->bpp); }

static void rect_header(client_t *c, int x, int y, int w, int h, int32_t enc)
{
    put16(c, x); put16(c, y); put16(c, w); put16(c, h); put32(c, (uint32_t)enc);
}

// ---------------------------------------------------------------- pixel format

static void set_format(client_t *c, const uint8_t *pf)
{
    int bits = pf[0], big = pf[2], truecolour = pf[3];
    int maxv[3] = {(pf[4] << 8) | pf[5], (pf[6] << 8) | pf[7], (pf[8] << 8) | pf[9]};
    int shift[3] = {pf[10], pf[11], pf[12]};
    if (!truecolour || (bits != 8 && bits != 16 && bits != 32)) {
        // colour-map formats are not supported: keep sending the server's own format
        ESP_LOGW(TAG, "viewer asked for an unsupported pixel format (%d bpp, true colour %d)", bits, truecolour);
        bits = 8; big = 0; maxv[0] = maxv[1] = maxv[2] = 3; shift[0] = 0; shift[1] = 2; shift[2] = 4;
    }
    c->bpp = bits / 8;
    for (int v = 0; v < 64; v++) {
        int ch[3] = {v & 3, (v >> 2) & 3, (v >> 4) & 3};
        uint32_t p = 0;
        for (int i = 0; i < 3; i++) p |= (uint32_t)((ch[i] * maxv[i] + 1) / 3) << shift[i];
        for (int b = 0; b < c->bpp; b++) {
            int sh = big ? 8 * (c->bpp - 1 - b) : 8 * b;
            c->px[v][b] = (uint8_t)(p >> sh);
        }
    }
    c->native = c->bpp == 1;
    for (int v = 0; v < 64; v++) if (c->px[v][0] != v) c->native = false;
    c->bg = c->fg = -1;
    // whatever the viewer has must be redrawn in the new format
    c->full = true;
    c->full_area = (rect_t){0, 0, FB_W, FB_H};
}

// ---------------------------------------------------------------- encoders

// Copies rows [y, y+h) x [x, x+w) of the screen into s_band, with the pointer composited when
// the viewer does not draw it itself.
static void fetch_band(client_t *c, int x, int y, int w, int h)
{
    const uint8_t *fb = vga_fb();
    for (int r = 0; r < h; r++) memcpy(s_band + r * w, fb + (y + r) * FB_W + x, w);
    if (c->sw_cur.w) {
        rect_t band = {x, y, w, h}, ov;
        if (!rect_intersect(&ov, &band, &c->sw_cur)) return;
        for (int yy = ov.y; yy < ov.y + ov.h; yy++)
            for (int xx = ov.x; xx < ov.x + ov.w; xx++) {
                uint8_t p = s_cur_img[(yy - c->sw_cur.y) * c->sw_cur.w + (xx - c->sw_cur.x)];
                if (p != 0xFF) s_band[(yy - y) * w + (xx - x)] = p;
            }
    }
}

static void encode_raw(client_t *c, const rect_t *r)
{
    rect_header(c, r->x, r->y, r->w, r->h, ENC_RAW);
    for (int y = r->y; y < r->y + r->h; y += 16) {
        int h = r->y + r->h - y < 16 ? r->y + r->h - y : 16;
        fetch_band(c, r->x, y, r->w, h);
        if (c->native) { put(c, s_band, r->w * h); continue; }   // server format: as is
        for (int i = 0; i < r->w * h; i++) put_px(c, s_band[i]);
    }
}

typedef struct { uint8_t colour, x, y, w, h; } subrect_t;

static void encode_tile(client_t *c, const uint8_t *src, int stride, int tw, int th)
{
    // histogram: the most common colour becomes the background
    uint16_t hist[64] = {0};
    int ncol = 0, other = -1;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            uint8_t v = src[y * stride + x] & 63;
            if (!hist[v]++) ncol++;
        }
    int bg = 0;
    for (int v = 1; v < 64; v++) if (hist[v] > hist[bg]) bg = v;
    if (ncol == 1) {
        if (bg != c->bg) { put8(c, HT_BG); put_px(c, bg); c->bg = bg; } else put8(c, 0);
        return;
    }
    int raw_size = 1 + tw * th * c->bpp;
    bool mono = ncol == 2;
    int per = mono ? 2 : 2 + c->bpp;
    int max_sub = (raw_size - 2 - 2 * c->bpp) / per;
    if (max_sub > 255) max_sub = 255;
    subrect_t sub[255];
    int n = 0;
    uint8_t done[16 * 16] = {0};
    for (int y = 0; y < th && n <= max_sub; y++)
        for (int x = 0; x < tw && n <= max_sub; x++) {
            uint8_t v = src[y * stride + x] & 63;
            if (v == bg || done[y * 16 + x]) continue;
            other = v;
            int w = 1;
            while (x + w < tw && (src[y * stride + x + w] & 63) == v && !done[y * 16 + x + w]) w++;
            int h = 1;
            for (; y + h < th; h++) {
                bool ok = true;
                for (int k = 0; k < w && ok; k++) ok = (src[(y + h) * stride + x + k] & 63) == v && !done[(y + h) * 16 + x + k];
                if (!ok) break;
            }
            for (int yy = y; yy < y + h; yy++) memset(done + yy * 16 + x, 1, w);
            if (n < max_sub) sub[n] = (subrect_t){v, x, y, w, h};
            n++;
        }
    if (n > max_sub) {
        put8(c, HT_RAW);
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) put_px(c, src[y * stride + x]);
        c->bg = c->fg = -1;
        return;
    }
    uint8_t flags = HT_ANY | (mono ? 0 : HT_COLOURED);
    if (bg != c->bg) flags |= HT_BG;
    if (mono && other != c->fg) flags |= HT_FG;
    put8(c, flags);
    if (flags & HT_BG) { put_px(c, bg); c->bg = bg; }
    if (flags & HT_FG) { put_px(c, other); c->fg = other; }
    if (!mono) c->fg = -1;
    put8(c, n);
    for (int i = 0; i < n; i++) {
        if (!mono) put_px(c, sub[i].colour);
        put8(c, (sub[i].x << 4) | sub[i].y);
        put8(c, ((sub[i].w - 1) << 4) | (sub[i].h - 1));
    }
}

static void encode_hextile(client_t *c, const rect_t *r)
{
    rect_header(c, r->x, r->y, r->w, r->h, ENC_HEXTILE);
    c->bg = c->fg = -1;   // nothing carries over between rectangles
    for (int y = r->y; y < r->y + r->h; y += 16) {
        int th = r->y + r->h - y < 16 ? r->y + r->h - y : 16;
        fetch_band(c, r->x, y, r->w, th);
        for (int x = 0; x < r->w; x += 16) {
            int tw = r->w - x < 16 ? r->w - x : 16;
            encode_tile(c, s_band + x, r->w, tw, th);
        }
    }
}

static void send_cursor_shape(client_t *c)
{
    int w, h, hx, hy;
    c->cur_serial = vga_cursor_image(s_cur_img, &w, &h, &hx, &hy);
    if (!w || !h) {
        // hidden: a single transparent pixel
        rect_header(c, 0, 0, 1, 1, ENC_CURSOR);
        put_px(c, 0);
        put8(c, 0);
        return;
    }
    rect_header(c, hx, hy, w, h, ENC_CURSOR);
    for (int i = 0; i < w * h; i++) put_px(c, s_cur_img[i] == 0xFF ? 0 : s_cur_img[i]);
    int stride = (w + 7) / 8;
    for (int y = 0; y < h; y++)
        for (int b = 0; b < stride; b++) {
            uint8_t m = 0;
            for (int k = 0; k < 8 && b * 8 + k < w; k++) if (s_cur_img[y * w + b * 8 + k] != 0xFF) m |= 0x80 >> k;
            put8(c, m);
        }
}

// The pointer sprite as composited for viewers without the Cursor encoding: damage its old and
// new places whenever it moves or changes.
static void track_sw_cursor(client_t *c)
{
    if (c->cursor_enc) { c->sw_cur.w = 0; return; }
    int x, y, w, h, hx, hy;
    bool shown;
    vga_cursor_get(&x, &y, &shown);
    uint32_t serial = vga_cursor_image(s_cur_img, &w, &h, &hx, &hy);
    rect_t now = {x - hx, y - hy, w, h};
    if (!shown || !w) now.w = now.h = 0;
    if (serial == c->cur_serial && now.x == c->sw_cur.x && now.y == c->sw_cur.y && now.w == c->sw_cur.w) return;
    if (c->sw_cur.w) vnc_damage(c->sw_cur.x, c->sw_cur.y, c->sw_cur.w, c->sw_cur.h);
    if (now.w) vnc_damage(now.x, now.y, now.w, now.h);
    c->sw_cur = now;
    c->cur_serial = serial;
}

static void send_update(client_t *c)
{
    rect_t list[MAX_DAMAGE];
    int n = damage_take(list);
    if (c->full) { list[0] = c->full_area; n = 1; }

    int px, py;
    bool shown;
    vga_cursor_get(&px, &py, &shown);
    int cw, ch, chx, chy;
    bool shape = c->cursor_enc && vga_cursor_image(s_cur_img, &cw, &ch, &chx, &chy) != c->cur_serial;
    bool pos = c->pointer_enc && (px != c->ptr_x || py != c->ptr_y);
    if (!n && !shape && !pos) return;

    put8(c, 0); put8(c, 0);
    put16(c, n + shape + pos);
    if (shape) send_cursor_shape(c);
    if (pos) { rect_header(c, px, py, 0, 0, ENC_POINTERPOS); c->ptr_x = px; c->ptr_y = py; }
    for (int i = 0; i < n && !c->err; i++) {
        if (c->hextile) encode_hextile(c, &list[i]); else encode_raw(c, &list[i]);
    }
    flush_out(c);
    c->want = c->full = false;
}

// ---------------------------------------------------------------- input from the viewer

static int keysym_usage(uint32_t ks, bool *printable)
{
    *printable = false;
    if (ks >= 0x20 && ks <= 0x7E) {
        uint8_t u;
        bool shift;
        if (!input_char_to_usage((char)ks, &u, &shift)) return 0;
        *printable = true;
        return u;
    }
    if (ks >= 0xFFBE && ks <= 0xFFC9) return K_F1 + (ks - 0xFFBE);
    if (ks >= 0xFFB1 && ks <= 0xFFB9) return 0x59 + (ks - 0xFFB1);   // keypad 1-9
    switch (ks) {
    case 0xFF08: return K_BKSP;
    case 0xFF09: case 0xFE20: return K_TAB;     // Tab, ISO_Left_Tab (Shift+Tab)
    case 0xFF0D: return K_ENTER;
    case 0xFF1B: return K_ESC;
    case 0xFFFF: return K_DEL;
    case 0xFF63: return K_INS;
    case 0xFF50: return K_HOME;
    case 0xFF57: return K_END;
    case 0xFF55: return K_PGUP;
    case 0xFF56: return K_PGDN;
    case 0xFF51: return K_LEFT;
    case 0xFF52: return K_UP;
    case 0xFF53: return K_RIGHT;
    case 0xFF54: return K_DOWN;
    case 0xFF67: return K_MENU;
    case 0xFF8D: return K_KP_ENTER;
    case 0xFFB0: return 0x62;                   // keypad 0
    case 0xFFAE: return 0x63;                   // keypad .
    case 0xFFAF: return 0x54;                   // keypad /
    case 0xFFAA: return 0x55;                   // keypad *
    case 0xFFAD: return 0x56;                   // keypad -
    case 0xFFAB: return 0x57;                   // keypad +
    case 0xFFE1: return K_LSHIFT;
    case 0xFFE2: return K_RSHIFT;
    case 0xFFE3: return K_LCTRL;
    case 0xFFE4: return K_RCTRL;
    case 0xFFE9: case 0xFFE7: return K_LALT;    // Alt_L, Meta_L
    case 0xFFEA: case 0xFFE8: return K_RALT;
    case 0xFFEB: return K_LGUI;
    case 0xFFEC: return K_RGUI;
    }
    // Caps Lock, AltGr (ISO_Level3_Shift), Num Lock...: the viewer has already applied them to
    // the keysyms it sends, so they are not passed on.
    return 0;
}

static void key_event(client_t *c, bool down, uint32_t ks)
{
    if ((ks & 0xFF000000u) == 0x01000000u && (ks & 0xFFFFFF) < 0x7F) ks &= 0xFFFFFF;   // Unicode keysym for ASCII
    if (!down) {
        bool found = false;
        for (int i = 0; i < MAX_KEYS; i++) {
            if (c->key_sym[i] == ks && c->key_usage[i]) {
                input_key(c->key_usage[i], false);
                c->key_usage[i] = 0;
                found = true;
            }
        }
        // Some viewers release a key under another keysym than they pressed it ('!' down, then
        // '1' up once Shift is gone): release the character keys still held so none gets stuck.
        if (!found && ks >= 0x20 && ks <= 0x7E) {
            for (int i = 0; i < MAX_KEYS; i++) {
                if (c->key_usage[i] && c->key_sym[i] >= 0x20 && c->key_sym[i] <= 0x7E) {
                    input_key(c->key_usage[i], false);
                    c->key_usage[i] = 0;
                }
            }
        }
        return;
    }
    bool printable;
    int u = keysym_usage(ks, &printable);
    if (!u) return;
    int slot = -1;
    for (int i = 0; i < MAX_KEYS; i++) if (c->key_sym[i] == ks && c->key_usage[i]) slot = i;   // auto-repeat
    if (slot < 0) for (int i = 0; i < MAX_KEYS; i++) if (!c->key_usage[i]) { slot = i; break; }
    if (slot < 0) return;
    c->key_sym[slot] = ks;
    c->key_usage[slot] = u;
    if (!printable) { input_key(u, true); return; }
    // The viewer sends the character typed with its own keyboard layout; the OS maps keys with
    // a US layout, so press the key that gives that character there.
    c->key_usage[slot] = input_char_down((char)(ks & 0x7F));
}

static void pointer_event(client_t *c, uint8_t mask, int x, int y)
{
    c->ptr_x = x; c->ptr_y = y;
    input_mouse_abs(x, y);
    // RFB: bit 0 left, 1 middle, 2 right, 3 wheel up, 4 wheel down
    uint8_t b = (mask & 1 ? BTN_LEFT : 0) | (mask & 2 ? BTN_MID : 0) | (mask & 4 ? BTN_RIGHT : 0);
    if (b != c->buttons) { input_mouse_buttons((input_buttons() & ~c->buttons) | b); c->buttons = b; }
    if (mask & 8) input_wheel(1);
    if (mask & 16) input_wheel(-1);
}

static void release_input(client_t *c)
{
    for (int i = 0; i < MAX_KEYS; i++) if (c->key_usage[i]) input_key(c->key_usage[i], false);
    if (c->buttons) input_mouse_buttons(input_buttons() & ~c->buttons);
    memset(c->key_usage, 0, sizeof(c->key_usage));
    c->buttons = 0;
}

// One client message; false when the connection must be closed.
static bool handle_message(client_t *c)
{
    uint8_t type, b[20];
    if (!rd(c, &type, 1)) return false;
    switch (type) {
    case 0:   // SetPixelFormat
        if (!rd(c, b, 19)) return false;
        set_format(c, b + 3);
        return true;
    case 2: { // SetEncodings
        if (!rd(c, b, 3)) return false;
        int n = (b[1] << 8) | b[2];
        c->hextile = c->cursor_enc = c->pointer_enc = false;
        for (int i = 0; i < n; i++) {
            if (!rd(c, b, 4)) return false;
            int32_t e = (int32_t)((uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
            if (e == ENC_HEXTILE) c->hextile = true;
            if (e == ENC_CURSOR) c->cursor_enc = true;
            if (e == ENC_POINTERPOS) c->pointer_enc = true;
        }
        c->cur_serial = 0;            // (re)send the pointer shape, or composite it
        c->ptr_x = c->ptr_y = -1;
        if (c->sw_cur.w) vnc_damage(c->sw_cur.x, c->sw_cur.y, c->sw_cur.w, c->sw_cur.h);
        c->sw_cur.w = 0;
        ESP_LOGI(TAG, "%s: hextile %d, cursor %d, pointer position %d", c->peer, c->hextile, c->cursor_enc, c->pointer_enc);
        return true;
    }
    case 3: { // FramebufferUpdateRequest
        if (!rd(c, b, 9)) return false;
        rect_t r = {(b[1] << 8) | b[2], (b[3] << 8) | b[4], (b[5] << 8) | b[6], (b[7] << 8) | b[8]};
        rect_t screen = {0, 0, FB_W, FB_H};
        c->want = true;
        if (!b[0]) {
            if (!rect_intersect(&r, &r, &screen)) r = screen;
            if (c->full) rect_union(&r, &r, &c->full_area);
            c->full = true;
            c->full_area = r;
        }
        return true;
    }
    case 4:   // KeyEvent
        if (!rd(c, b, 7)) return false;
        key_event(c, b[0], (uint32_t)b[3] << 24 | b[4] << 16 | b[5] << 8 | b[6]);
        return true;
    case 5:   // PointerEvent
        if (!rd(c, b, 5)) return false;
        pointer_event(c, b[0], (b[1] << 8) | b[2], (b[3] << 8) | b[4]);
        return true;
    case 6: { // ClientCutText (ignored; a negative length is the extended clipboard format)
        if (!rd(c, b, 7)) return false;
        int32_t len = (int32_t)((uint32_t)b[3] << 24 | b[4] << 16 | b[5] << 8 | b[6]);
        uint32_t n = len < 0 ? (uint32_t)-len : (uint32_t)len;
        return n <= 1 << 20 && skip(c, n);
    }
    }
    ESP_LOGW(TAG, "%s: unknown message %d, closing", c->peer, type);
    return false;
}

// ---------------------------------------------------------------- connection setup

static bool handshake(client_t *c)
{
    static const char version[] = "RFB 003.008\n";
    char v[13] = {0};
    if (send(c->fd, version, 12, 0) != 12 || !rd(c, v, 12)) return false;
    int minor = 8;
    if (!strncmp(v, "RFB 003.003", 11)) minor = 3;
    else if (!strncmp(v, "RFB 003.007", 11)) minor = 7;
    else if (strncmp(v, "RFB 003.", 8)) return false;
    const char *pw = CONFIG_BEVEL_VNC_PASSWORD;
    uint8_t sec = pw[0] ? 2 : 1;
    if (minor == 3) {
        put32(c, sec);
    } else {
        put8(c, 1); put8(c, sec);
        flush_out(c);
        uint8_t chosen;
        if (!rd(c, &chosen, 1) || chosen != sec) return false;
    }
    flush_out(c);
    if (sec == 2) {
        uint8_t challenge[16], resp[16], expect[16];
        for (int i = 0; i < 16; i += 4) { uint32_t r = esp_random(); memcpy(challenge + i, &r, 4); }
        if (send(c->fd, challenge, 16, 0) != 16 || !rd(c, resp, 16)) return false;
        vnc_auth_response(pw, challenge, expect);
        bool ok = !memcmp(resp, expect, 16);
        put32(c, ok ? 0 : 1);
        if (!ok && minor == 8) { static const char why[] = "Wrong password"; put32(c, sizeof(why) - 1); put(c, why, sizeof(why) - 1); }
        flush_out(c);
        if (!ok) { ESP_LOGW(TAG, "%s: wrong password", c->peer); vTaskDelay(pdMS_TO_TICKS(1000)); return false; }
    } else if (minor == 8) {
        put32(c, 0);
        flush_out(c);
    }
    uint8_t shared;
    if (!rd(c, &shared, 1)) return false;
    // ServerInit: our native format is RGB222, one byte per pixel
    static const char name[] = OS_NAME;
    put16(c, FB_W); put16(c, FB_H);
    static const uint8_t pf[16] = {8, 6, 0, 1, 0, 3, 0, 3, 0, 3, 0, 2, 4, 0, 0, 0};
    put(c, pf, 16);
    put32(c, sizeof(name) - 1);
    put(c, name, sizeof(name) - 1);
    flush_out(c);
    set_format(c, pf);
    return !c->err;
}

static void serve(client_t *c, int listen_fd)
{
    struct timeval tv = {10, 0};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (!handshake(c)) { ESP_LOGW(TAG, "%s: handshake failed", c->peer); return; }
    ESP_LOGI(TAG, "viewer %s connected", c->peer);
    strlcpy(s_peer, c->peer, sizeof(s_peer));
    s_state = 2;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ndmg = 0;
    s_active = true;
    xSemaphoreGive(s_lock);
    while (!c->err) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(c->fd, &fds);
        FD_SET(listen_fd, &fds);
        struct timeval to = {0, 15000};    // also the pace of updates: at most ~60 per second
        int maxfd = c->fd > listen_fd ? c->fd : listen_fd;
        int r = select(maxfd + 1, &fds, NULL, NULL, &to);
        if (r < 0 && errno != EINTR) break;
        if (r > 0 && FD_ISSET(listen_fd, &fds)) break;          // a new viewer takes over
        if (r > 0 && FD_ISSET(c->fd, &fds) && !handle_message(c)) break;
        track_sw_cursor(c);
        if (c->want) send_update(c);
    }
    s_active = false;
    release_input(c);
    ESP_LOGI(TAG, "viewer %s disconnected", c->peer);
}

static void vnc_task(void *arg)
{
    // wait for an address
    for (;;) {
        net_status_t ns;
        net_status(&ns);
        if (ns.state == NET_CONNECTED) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CONFIG_BEVEL_VNC_PORT), .sin_addr.s_addr = htonl(VNC_BIND)};
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 2) != 0) {
        ESP_LOGE(TAG, "cannot listen on port %d (errno %d)", CONFIG_BEVEL_VNC_PORT, errno);
        s_state = -1;
        vTaskDelete(NULL);
        return;
    }
    s_state = 1;
    char st[96];
    vnc_status(st, sizeof(st));
    ESP_LOGI(TAG, "%s%s", st, CONFIG_BEVEL_VNC_PASSWORD[0] ? "" : " (no password)");
    serial_printf("@@VNC %d\n", CONFIG_BEVEL_VNC_PORT);
    static client_t c;
    int pending = -1;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = pending >= 0 ? pending : accept(lfd, (struct sockaddr *)&peer, &plen);
        pending = -1;
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0) memset(&peer, 0, sizeof(peer));
        memset(&c, 0, sizeof(c));
        c.out = s_out;
        c.fd = fd;
        snprintf(c.peer, sizeof(c.peer), "%s", inet_ntoa(peer.sin_addr));
        serve(&c, lfd);
        close(fd);
        s_state = 1;
        // a viewer waiting in the backlog (the one that took over) is served next
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(lfd, &fds);
        struct timeval to = {0, 0};
        if (select(lfd + 1, &fds, NULL, NULL, &to) > 0) { plen = sizeof(peer); pending = accept(lfd, (struct sockaddr *)&peer, &plen); }
    }
}

void vnc_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    s_lock = xSemaphoreCreateMutex();
    s_band = heap_caps_malloc(FB_W * 16, MALLOC_CAP_INTERNAL);
    s_out = heap_caps_malloc(OUT_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_band || !s_out) { ESP_LOGE(TAG, "not enough memory"); return; }
    xTaskCreatePinnedToCore(vnc_task, "vnc", 6144, NULL, 3, NULL, SYS_CORE);
}

#endif

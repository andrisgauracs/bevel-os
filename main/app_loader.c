// Loadable apps: ELF files from the drive, run with Espressif's elf_loader.
//
// Each app runs in its own task on the system core and talks to the OS only through the
// function table in apps/bevel.h. It draws into a private canvas (PSRAM) that the compositor
// blits, so a slow or hung app never blocks the desktop. Faults inside an app task are caught
// and turned into an error dialog; the task list can end an app that stopped responding.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_elf.h"
#include "private/elf_symbol.h"
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include "esp_attr.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "riscv/rvruntime-frames.h"
#include "riscv/csr.h"
#else
#include "esp_ipc.h"
#include "xtensa_api.h"
#include "xtensa_context.h"
#endif
#include "display.h"
#include "audio.h"
#include "wm.h"
#include "apps.h"
#include "boot.h"
#include "os.h"
#include "fsutil.h"
#include "comp.h"
#include "../apps/bevel.h"

#define MAX_APPS 4
#define MAX_APP_FILES 8
#define APP_MAGIC 0x42564C31  // "BVL1"

typedef struct mem_hdr { struct mem_hdr *next, *prev; uint32_t size; uint32_t magic; } mem_hdr_t;

typedef struct {
    bool used, crashed, closing;
    int id;
    char name[40], path[300];
    TaskHandle_t task;
    esp_elf_t elf;
    window_t *win;
    widget_t *canvas_w;
    uint8_t *canvas;
    int cw, ch;
    int overlay;                // true-colour layer over the canvas, or -1
    gfx_t g;
    QueueHandle_t q;
    volatile int in_api;
    volatile uint32_t last_poll_ms;
    mem_hdr_t *mem;
    FILE *files[MAX_APP_FILES];
    uint32_t mem_bytes, image_bytes;
    uint32_t crash_cause, crash_pc, crash_addr;
} app_t;

static app_t s_apps[MAX_APPS];
static int s_next_id = 1;
static const bevel_api_t s_api;
static void *api_alloc(int bytes);
static void api_free(void *p);

static app_t *cur_app(void)
{
    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < MAX_APPS; i++) if (s_apps[i].used && s_apps[i].task == t) return &s_apps[i];
    return NULL;
}

static app_t *app_by_id(int id)
{
    for (int i = 0; i < MAX_APPS; i++) if (s_apps[i].used && s_apps[i].id == id) return &s_apps[i];
    return NULL;
}

#define ENTER app_t *a = cur_app(); if (!a) return; a->in_api++
#define ENTER_R(r) app_t *a = cur_app(); if (!a) return r; a->in_api++
#define LEAVE a->in_api--

// ---------------------------------------------------------------- GUI side of an app window

static void push_event(app_t *a, const bv_event_t *e)
{
    if (a->q) xQueueSend(a->q, e, 0);
}

static void cv_paint(widget_t *w, gfx_t *g)
{
    app_t *a = w->win->user;
    if (!a || !a->canvas) return;
    if (a->overlay >= 0) disp_overlay_place(a->overlay, g->ox, g->oy);
    gfx_blit(g, 0, 0, a->cw, a->ch, a->canvas, a->cw);
}

static bool cv_key(widget_t *w, const event_t *e)
{
    app_t *a = w->win->user;
    if (!a) return false;
    if (e->mods & MOD_ALT) return false;
    bv_event_t b = {.type = e->down ? BV_KEY_DOWN : BV_KEY_UP, .key = e->key, .ch = e->ch, .mods = e->mods & 7, .repeat = e->repeat};
    push_event(a, &b);
    return true;
}

static void cv_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    app_t *a = w->win->user;
    if (!a || type == EV_WHEEL) return;
    bv_event_t b = {.type = type == EV_MOUSE_DOWN ? BV_MOUSE_DOWN : type == EV_MOUSE_UP ? BV_MOUSE_UP : BV_MOUSE_MOVE,
                    .button = btn, .x = x, .y = y};
    if (b.type == BV_MOUSE_MOVE && a->q && uxQueueMessagesWaiting(a->q) > 8) return; // do not flood a slow app
    push_event(a, &b);
}
static const canvas_ops_t cv_ops = {cv_paint, cv_key, cv_mouse};

static bool appwin_close(window_t *w)
{
    app_t *a = w->user;
    if (!a) return true;
    bv_event_t b = {.type = BV_CLOSE};
    push_event(a, &b);
    a->closing = true;
    return false; // the app closes its window by returning from app_main
}

typedef struct { app_t *a; const char *title; int w, h, res; } open_req_t;

static void gui_open(void *arg)
{
    open_req_t *r = arg;
    app_t *a = r->a;
    r->res = -1;
    if (a->win) return;
    if (r->w < 64) r->w = 64;
    if (r->h < 32) r->h = 32;
    if (r->w > SCREEN_W) r->w = SCREEN_W;
    if (r->h > DESK_H - TITLE_H - 2 * BORDER_W - 1) r->h = DESK_H - TITLE_H - 2 * BORDER_W - 1;
    a->canvas = heap_caps_aligned_alloc(16, r->w * r->h + 16, MALLOC_CAP_SPIRAM);
    if (!a->canvas) return;
    memset(a->canvas, 0, r->w * r->h);
    a->cw = r->w; a->ch = r->h;
    gfx_init(&a->g, a->canvas, a->cw, 0, 0, a->cw, a->ch);
    window_t *w = wm_create(r->title, r->w, r->h, 0);
    if (!w) return;
    w->icon = strcasestr(a->name, "snake") || strcasestr(a->name, "doom") ? &icon16_game : &icon16_app;
    w->user = a;
    w->task_id = a->id;
    w->on_close = appwin_close;
    a->canvas_w = ui_canvas(w, 0, 0, r->w, r->h, &cv_ops, true);
    a->win = w;
    if (w->r.w > SCREEN_W) { w->r.x = (SCREEN_W - w->r.w) / 2; w->r.y = 8; }   // full-width canvas: borders hang off the sides
    wm_show(w);
    r->res = 0;
}

static void gui_title(void *arg)
{
    open_req_t *r = arg;
    if (r->a->win) wm_set_title(r->a->win, r->title);
}

static void gui_invalidate(void *arg)
{
    app_t *a = arg;
    if (a->used && a->win && a->canvas_w) ui_invalidate(a->canvas_w);
}

typedef struct { app_t *a; int x, y, w, h; } rect_req_t;
static void gui_invalidate_rect(void *arg)
{
    rect_req_t *r = arg;
    app_t *a = r->a;
    if (a->used && a->win && !(a->win->flags & (WIN_MINIMIZED | WIN_HIDDEN | WIN_DEAD))) {
        rect_t c;
        wm_client_rect(a->win, &c);
        rect_t d = {c.x + r->x, c.y + r->y, r->w, r->h};
        if (rect_intersect(&d, &d, &c)) comp_invalidate_rect(&d);
    }
    free(r);
}

typedef struct { app_t *a; const char *title, *text; int buttons, res; } mb_req_t;
static void gui_msgbox(void *arg)
{
    mb_req_t *r = arg;
    r->res = msgbox(r->a->win, r->title, r->text, MB_INFO, r->buttons == BV_MB_YESNO ? MB_YESNO : MB_OK);
}

// ---------------------------------------------------------------- API (runs in the app's task)

static int api_win_open(const char *title, int w, int h)
{
    ENTER_R(-1);
    open_req_t r = {a, title ? title : a->name, w, h, -1};
    ev_call_sync(gui_open, &r);
    LEAVE;
    return r.res;
}

static void api_win_title(const char *title)
{
    ENTER;
    open_req_t r = {a, title, 0, 0, 0};
    ev_call_sync(gui_title, &r);
    LEAVE;
}

static int api_event(bv_event_t *ev, int timeout_ms)
{
    app_t *a = cur_app();
    if (!a || !ev) return 0;
    a->last_poll_ms = os_uptime_ms();
    int got = xQueueReceive(a->q, ev, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    a->last_poll_ms = os_uptime_ms();
    return got;
}

#define DRAW(stmt) do { app_t *a = cur_app(); if (a && a->canvas) { stmt; } } while (0)
static void api_clear(int c) { DRAW(gfx_fill(&a->g, 0, 0, a->cw, a->ch, c & 63)); }
static void api_fill(int x, int y, int w, int h, int c) { DRAW(gfx_fill(&a->g, x, y, w, h, c & 63)); }
static void api_frame(int x, int y, int w, int h, int c) { DRAW(gfx_frame(&a->g, x, y, w, h, c & 63)); }
static void api_pixel(int x, int y, int c) { DRAW(gfx_pixel(&a->g, x, y, c & 63)); }
static void api_line(int x0, int y0, int x1, int y1, int c)
{
    // clamp wild coordinates so a buggy app cannot spin here for seconds
    if (x0 < -2000 || x0 > 2000 || x1 < -2000 || x1 > 2000 || y0 < -2000 || y0 > 2000 || y1 < -2000 || y1 > 2000) return;
    DRAW(gfx_line(&a->g, x0, y0, x1, y1, c & 63));
}
static int api_text(int x, int y, const char *s, int font, int c)
{
    app_t *a = cur_app();
    if (!a || !a->canvas || !s) return 0;
    return gfx_text(&a->g, x, y, s, -1, font < 0 || font > 2 ? 0 : font, c & 63);
}
static int api_text_width(const char *s, int font) { return s ? gfx_text_w(s, -1, font < 0 || font > 2 ? 0 : font) : 0; }
static void api_blit(int x, int y, int w, int h, const uint8_t *px)
{
    if (!px || w <= 0 || h <= 0 || w > 255 || h > 255) return;
    bitmap_t b = {w, h, 0, 0, px};
    DRAW(gfx_bitmap(&a->g, x, y, &b));
}
static void api_bevel(int x, int y, int w, int h, int raised)
{
    DRAW(if (raised) ui_draw_raised(&a->g, x, y, w, h); else ui_draw_sunken(&a->g, x, y, w, h));
}
static void api_flush(void)
{
    app_t *a = cur_app();
    if (a) ev_call(gui_invalidate, a);
}
static void api_flush_rect(int x, int y, int w, int h)
{
    app_t *a = cur_app();
    if (!a) return;
    rect_req_t *r = malloc(sizeof(*r));
    if (!r) return;
    *r = (rect_req_t){a, x, y, w, h};
    ev_call(gui_invalidate_rect, r);
}

static bool app_path(const char *in, char *out, int n)
{
    if (!in || strstr(in, "..")) return false;
    snprintf(out, n, "%s%s%s", DISK_ROOT, in[0] == '/' ? "" : "/", in);
    return true;
}

static int api_file_read(const char *path, void *buf, int max)
{
    ENTER_R(-1);
    char p[300];
    int n = -1;
    if (buf && max > 0 && app_path(path, p, sizeof(p))) {
        FILE *f = fopen(p, "rb");
        if (f) { n = fread(buf, 1, max, f); fclose(f); }
    }
    LEAVE;
    return n;
}

static int api_file_write(const char *path, const void *buf, int len)
{
    ENTER_R(-1);
    char p[300];
    int res = -1;
    if (buf && len >= 0 && app_path(path, p, sizeof(p))) {
        FILE *f = fopen(p, "wb");
        if (f) { res = fwrite(buf, 1, len, f) == (size_t)len ? 0 : -1; if (fclose(f) != 0) res = -1; }
    }
    LEAVE;
    return res;
}

static uint32_t api_ticks(void) { return os_uptime_ms(); }
static void api_sleep(int ms) { if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms > 10000 ? 10000 : ms)); else taskYIELD(); }
static uint32_t api_random(void) { return esp_random(); }

static void *api_alloc(int bytes)
{
    ENTER_R(NULL);
    void *res = NULL;
    if (bytes > 0 && bytes <= BOARD_APP_MEM_MAX) {
        mem_hdr_t *h = heap_caps_malloc(sizeof(mem_hdr_t) + bytes, MALLOC_CAP_SPIRAM);
        if (h) {
            h->size = bytes; h->magic = APP_MAGIC; h->prev = NULL; h->next = a->mem;
            if (a->mem) a->mem->prev = h;
            a->mem = h;
            a->mem_bytes += bytes;
            res = h + 1;
        }
    }
    LEAVE;
    return res;
}

static void api_free(void *p)
{
    if (!p) return;
    ENTER;
    mem_hdr_t *h = (mem_hdr_t *)p - 1;
    if (h->magic == APP_MAGIC) {   // ignore pointers that did not come from alloc
        h->magic = 0;
        if (h->prev) h->prev->next = h->next; else a->mem = h->next;
        if (h->next) h->next->prev = h->prev;
        a->mem_bytes -= h->size;
        free(h);
    }
    LEAVE;
}

static int api_msgbox(const char *title, const char *text, int buttons)
{
    ENTER_R(0);
    mb_req_t r = {a, title ? title : a->name, text ? text : "", buttons, 0};
    ev_call_sync(gui_msgbox, &r);
    a->last_poll_ms = os_uptime_ms();
    LEAVE;
    return r.res;
}

static void api_log(const char *text) { if (text) serial_printf("[app] %.200s\n", text); }

static uint8_t *api_canvas(int *w, int *h)
{
    app_t *a = cur_app();
    if (!a || !a->canvas) return NULL;
    if (w) *w = a->cw;
    if (h) *h = a->ch;
    return a->canvas;
}

// --- API version 2: sound and true colour

static int api_snd_play(const void *pcm, int frames, int rate, int format, int volume, int pan)
{
    app_t *a = cur_app();
    return a ? audio_play(pcm, frames, rate, format, volume, pan, a) : 0;
}
static void api_snd_set(int voice, int volume, int pan) { audio_voice_set(voice, volume, pan); }
static int api_snd_playing(int voice) { return audio_playing(voice); }
static void api_snd_stop(int voice) { audio_stop(voice); }

static void gui_key_canvas(void *arg)
{
    app_t *a = arg;
    if (a->used && a->canvas && a->overlay >= 0) {
        memset(a->canvas, DISP_OVERLAY_KEY(a->overlay), a->cw * a->ch);
        if (a->canvas_w) ui_invalidate(a->canvas_w);
    }
}

static uint16_t *api_canvas16(int *w, int *h)
{
    ENTER_R(NULL);
    uint16_t *px = NULL;
    if (a->canvas) {
        if (a->overlay < 0) {
            a->overlay = disp_overlay_alloc(a->cw, a->ch);
            if (a->overlay >= 0) ev_call_sync(gui_key_canvas, a);   // the canvas now only says where the layer shows
        }
        px = disp_overlay_px(a->overlay, w, h);
    }
    LEAVE;
    return px;
}

static void api_flush16(void)
{
    app_t *a = cur_app();
    if (a && a->overlay >= 0) disp_overlay_update(a->overlay);
}

// ---------------------------------------------------------------- C library for apps
// Apps may also link against the C library. Memory and files go through these wrappers so that
// everything is returned when the app ends or is ended, and exit() ends the app, not the OS.

static void *app_malloc(size_t n) { return api_alloc((int)n); }
static void app_free(void *p) { api_free(p); }
static void *app_calloc(size_t n, size_t m)
{
    size_t t = n * m;
    void *p = api_alloc((int)t);
    if (p) memset(p, 0, t);
    return p;
}
static void *app_realloc(void *p, size_t n)
{
    if (!p) return api_alloc((int)n);
    if (!n) { api_free(p); return NULL; }
    mem_hdr_t *h = (mem_hdr_t *)p - 1;
    if (h->magic != APP_MAGIC) return NULL;
    if (h->size >= n) return p;
    void *q = api_alloc((int)n);
    if (q) { memcpy(q, p, h->size); api_free(p); }
    return q;
}
static char *app_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = api_alloc((int)n);
    if (d) memcpy(d, s, n);
    return d;
}

// relative paths land in /disk/System (config files, saved games)
static const char *fix_path(const char *in, char *buf, int n)
{
    if (in[0] == '/') return in;
    while (in[0] == '.' && in[1] == '/') in += 2;
    snprintf(buf, n, DISK_ROOT "/System/%s", in);
    return buf;
}

static FILE *app_fopen(const char *path, const char *mode)
{
    ENTER_R(NULL);
    char buf[300];
    FILE *f = fopen(fix_path(path, buf, sizeof(buf)), mode);
    if (f) {
        int i;
        for (i = 0; i < MAX_APP_FILES; i++) if (!a->files[i]) { a->files[i] = f; break; }
        if (i == MAX_APP_FILES) { fclose(f); f = NULL; }
    }
    LEAVE;
    return f;
}

static int app_fclose(FILE *f)
{
    ENTER_R(-1);
    int r = -1;
    for (int i = 0; i < MAX_APP_FILES; i++) if (a->files[i] == f) { a->files[i] = NULL; r = fclose(f); break; }
    LEAVE;
    return r;
}

static int app_mkdir(const char *path, mode_t mode) { char b[300]; return mkdir(fix_path(path, b, sizeof(b)), mode); }
static int app_remove(const char *path) { char b[300]; return remove(fix_path(path, b, sizeof(b))); }
static int app_rename(const char *x, const char *y)
{
    char b1[300], b2[300];
    y = fix_path(y, b2, sizeof(b2));
    unlink(y);
    return rename(fix_path(x, b1, sizeof(b1)), y);
}
static int app_no_system(const char *cmd) { return -1; }
static void gui_app_exited(void *arg);
static void app_exit(int code)
{
    app_t *a = cur_app();
    if (a) ev_call(gui_app_exited, a);
    vTaskDelete(NULL);
    for (;;) { }
}
// file I/O marks the task as "inside the OS" so it is never ended while holding the drive's lock
static size_t app_fread(void *p, size_t s, size_t n, FILE *f) { ENTER_R(0); size_t r = fread(p, s, n, f); LEAVE; return r; }
static size_t app_fwrite(const void *p, size_t s, size_t n, FILE *f) { ENTER_R(0); size_t r = fwrite(p, s, n, f); LEAVE; return r; }
static int app_fseek(FILE *f, long o, int w) { ENTER_R(-1); int r = fseek(f, o, w); LEAVE; return r; }
static int app_putchar(int c) { return c; }
static int app_puts(const char *t) { return 0; }
static int app_printf(const char *fmt, ...) { return 0; }
static int app_fprintf(FILE *f, const char *fmt, ...)
{
    if (f == stdout || f == stderr) return 0;
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
static int app_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (f == stdout || f == stderr) return 0;
    return vfprintf(f, fmt, ap);
}

// Compiler support routines (64-bit division, double-precision arithmetic). The RISC-V toolchain's
// libgcc is not position-independent, so apps built for the P4 import these from the OS instead of
// linking libgcc; the Xtensa apps for the S3 link their own copy (tools/build_apps.sh).
#define LIBGCC_LIST(X) X(__divdi3) X(__udivdi3) X(__moddi3) X(__umoddi3) X(__muldi3) X(__ashldi3) X(__lshrdi3) X(__ashrdi3) X(__clzsi2) X(__ctzsi2) X(__adddf3) X(__subdf3) X(__muldf3) X(__divdf3) X(__extendsfdf2) X(__truncdfsf2) X(__fixdfsi) X(__fixunsdfsi) X(__floatsidf) X(__floatunsidf) X(__floatdidf) X(__fixdfdi) X(__eqdf2) X(__nedf2) X(__ltdf2) X(__ledf2) X(__gtdf2) X(__gedf2) X(__unorddf2)
#define X(n) extern void n(void);
LIBGCC_LIST(X)
#undef X

static const struct { const char *name; void *fn; } s_overrides[] = {
    {"malloc", app_malloc}, {"free", app_free}, {"calloc", app_calloc}, {"realloc", app_realloc}, {"strdup", app_strdup},
    {"fopen", app_fopen}, {"fclose", app_fclose}, {"fread", app_fread}, {"fwrite", app_fwrite}, {"fseek", app_fseek},
    {"mkdir", app_mkdir}, {"remove", app_remove}, {"rename", app_rename}, {"system", app_no_system}, {"exit", app_exit},
    {"putchar", app_putchar}, {"puts", app_puts}, {"printf", app_printf}, {"fprintf", app_fprintf}, {"vfprintf", app_vfprintf},
    // the rest of what apps may import, straight from the C library
    {"ftell", ftell}, {"fflush", fflush}, {"memcpy", memcpy}, {"memmove", memmove}, {"memset", memset}, {"memcmp", memcmp},
    {"strlen", strlen}, {"strcmp", strcmp}, {"strncmp", strncmp}, {"strcasecmp", strcasecmp}, {"strncasecmp", strncasecmp},
    {"strchr", strchr}, {"strrchr", strrchr}, {"strncpy", strncpy}, {"strcpy", strcpy}, {"strcat", strcat}, {"strstr", strstr},
    {"atoi", atoi}, {"snprintf", snprintf}, {"vsnprintf", vsnprintf}, {"sscanf", sscanf}, {"abs", abs},
    {"__errno", __errno}, {"__getreent", __getreent}, {"_ctype_", (void *)_ctype_},
#define X(n) {#n, n},
    LIBGCC_LIST(X)
#undef X
};

static uintptr_t app_resolver(const char *name)
{
    for (int i = 0; i < (int)(sizeof(s_overrides) / sizeof(s_overrides[0])); i++) {
        if (!strcmp(name, s_overrides[i].name)) return (uintptr_t)s_overrides[i].fn;
    }
    serial_printf("[app] symbol not provided by the OS: %s\n", name);
    return 0;
}

static const bevel_api_t s_api = {
    BEVEL_API_VERSION, api_win_open, api_win_title, api_event, api_clear, api_fill, api_frame, api_pixel, api_line,
    api_text, api_text_width, api_blit, api_bevel, api_flush, api_flush_rect, api_file_read, api_file_write,
    api_ticks, api_sleep, api_random, api_alloc, api_free, api_msgbox, api_log, api_canvas,
    api_snd_play, api_snd_set, api_snd_playing, api_snd_stop, api_canvas16, api_flush16,
};

// ---------------------------------------------------------------- lifecycle

static void gui_cleanup(void *arg)
{
    app_t *a = arg;
    if (a->win) { a->win->user = NULL; a->win->on_close = NULL; wm_destroy(a->win); a->win = NULL; }
}

// Free everything an app owned. GUI task, after the app's task is gone.
static void app_release(app_t *a)
{
    audio_stop_owner(a);
    gui_cleanup(a);
    if (a->overlay >= 0) { disp_overlay_free(a->overlay); a->overlay = -1; }
    for (int i = 0; i < MAX_APP_FILES; i++) if (a->files[i]) { fclose(a->files[i]); a->files[i] = NULL; }
    for (mem_hdr_t *h = a->mem; h;) { mem_hdr_t *n = h->next; free(h); h = n; }
    a->mem = NULL;
    free(a->canvas);
    a->canvas = NULL;
    esp_elf_deinit(&a->elf);
    if (a->q) { vQueueDelete(a->q); a->q = NULL; }
    a->used = false;
}

static void gui_app_exited(void *arg)
{
    app_t *a = arg;
    if (!a->used) return;
    vTaskDelay(pdMS_TO_TICKS(20)); // let the task finish deleting itself
    app_release(a);
}

static void app_task(void *arg)
{
    app_t *a = arg;
    char *argv[2] = {(char *)&s_api, a->path};
    a->last_poll_ms = os_uptime_ms();
    esp_elf_request(&a->elf, 0, APP_MAGIC, argv);
    ev_call(gui_app_exited, a);
    vTaskDelete(NULL);
}

static void gui_app_crashed(void *arg)
{
    app_t *a = arg;
    if (!a->used) return;
    char name[40], msg[200];
    strlcpy(name, a->name, sizeof(name));
#if CONFIG_IDF_TARGET_ESP32P4
    static const char *const causes[] = {[0] = "a jump to an invalid address", [1] = "a jump to an invalid address",
        [2] = "an illegal instruction", [3] = "a breakpoint", [4] = "an unaligned memory access", [5] = "reading an invalid address",
        [6] = "an unaligned memory access", [7] = "writing to an invalid address"};
    const char *why = a->crash_cause < 8 && causes[a->crash_cause] ? causes[a->crash_cause] : "an error";
#else
    static const char *const causes[] = {[0] = "an illegal instruction", [2] = "a bad instruction address", [3] = "a memory access error",
        [6] = "a division by zero", [9] = "an unaligned memory access", [20] = "a jump to an invalid address",
        [28] = "reading an invalid address", [29] = "writing to an invalid address"};
    const char *why = a->crash_cause < 30 && causes[a->crash_cause] ? causes[a->crash_cause] : "an error";
#endif
    snprintf(msg, sizeof(msg), "%s has stopped because of %s (at 0x%08x, address 0x%08x).\n\nIt was closed. Everything else keeps running.",
             name, why, (unsigned)a->crash_pc, (unsigned)a->crash_addr);
    serial_printf("[app] %s crashed: cause %u pc %08x addr %08x\n", name, (unsigned)a->crash_cause, (unsigned)a->crash_pc, (unsigned)a->crash_addr);
    vTaskDelete(a->task);
    app_release(a);
    msgbox(NULL, "Program error", msg, MB_ERROR, MB_OK);
}

// The faulting app task resumes here instead of at the bad instruction.
static void app_crash_landing(void)
{
    app_t *a = cur_app();
    if (a) { a->crashed = true; ev_call(gui_app_crashed, a); }
    for (;;) vTaskSuspend(NULL);
}

#if CONFIG_IDF_TARGET_ESP32P4
// Every CPU exception that is not an interrupt passes through here (the linker wraps ESP-IDF's
// xt_unhandled_exception, see main/CMakeLists.txt). A fault in an app's own code is turned into a
// jump to the landing function; anything else goes on to the normal panic handler.
extern void __real_xt_unhandled_exception(void *frame);
void IRAM_ATTR __wrap_xt_unhandled_exception(void *frame)
{
    RvExcFrame *f = frame;
    app_t *a = xPortInIsrContext() ? NULL : cur_app();
    if (!a || a->crashed || a->in_api || f->mcause >= 8) { __real_xt_unhandled_exception(frame); return; }
    a->crash_cause = f->mcause;
    a->crash_pc = f->mepc;
    a->crash_addr = RV_READ_CSR(mtval);
    f->mepc = (uint32_t)app_crash_landing;
}
#else
// On the S3 the exception table is per core, so a handler is installed on each of them; a fault in
// an app's own code jumps to the landing function, anything else goes on to the panic handler.
static void app_exception(XtExcFrame *frame)
{
    extern void xt_unhandled_exception(XtExcFrame *frame);
    app_t *a = xPortInIsrContext() ? NULL : cur_app();
    if (!a || a->crashed || a->in_api) { xt_unhandled_exception(frame); return; }
    a->crash_cause = frame->exccause;
    a->crash_pc = frame->pc;
    a->crash_addr = frame->excvaddr;
    frame->pc = (uint32_t)app_crash_landing;
    frame->ps &= ~(3u << 16);       // CALLINC = 0, so the landing function's ENTRY does not rotate the window
    frame->lcount = 0;
}

static void install_handlers(void *arg)
{
    static const int causes[] = {0, 2, 3, 6, 9, 20, 28, 29};
    for (int i = 0; i < (int)(sizeof(causes) / sizeof(causes[0])); i++) xt_set_exception_handler(causes[i], app_exception);
}
#endif

void app_loader_init(void)
{
    elf_set_symbol_resolver(app_resolver);
#if CONFIG_IDF_TARGET_ESP32S3
    install_handlers(NULL);                                     // this core
    esp_ipc_call_blocking(1 - xPortGetCoreID(), install_handlers, NULL);   // and the other one
#endif
}

void os_run_elf(const char *path)
{
    app_t *a = NULL;
    for (int i = 0; i < MAX_APPS; i++) if (!s_apps[i].used) { a = &s_apps[i]; break; }
    if (!a) { msgbox(NULL, "Programs", "Too many programs are running. Close one first.", MB_WARN, MB_OK); return; }
    wm_busy(true);
    wm_pump(0);
    int len = 0;
    char *img = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        img = heap_caps_aligned_alloc(4, len + 4, MALLOC_CAP_SPIRAM);
        if (img && fread(img, 1, len, f) != (size_t)len) { free(img); img = NULL; }
        fclose(f);
    }
    const char *err = NULL;
    memset(a, 0, sizeof(*a));
    a->overlay = -1;
    if (!img) err = "The program file could not be read.";
    else if (len < 52 || memcmp(img, "\x7f" "ELF", 4)) err = "This file is not a program for Bevel OS.";
    else {
        esp_elf_init(&a->elf);
        if (esp_elf_relocate(&a->elf, (const uint8_t *)img, len) != 0) { esp_elf_deinit(&a->elf); err = "The program could not be loaded (it may be damaged or built for another system)."; }
    }
    free(img);
    wm_busy(false);
    if (err) { msgbox(NULL, "Programs", err, MB_ERROR, MB_OK); return; }
    strlcpy(a->path, path, sizeof(a->path));
    strlcpy(a->name, fs_basename(path), sizeof(a->name));
    char *dot = strrchr(a->name, '.');
    if (dot) *dot = 0;
    a->id = s_next_id++;
    a->image_bytes = len;
    a->q = xQueueCreate(32, sizeof(bv_event_t));
    a->used = true;
    // big programs (Doom) recurse deeply; the stack must be internal RAM because the drive is flash
    if (xTaskCreatePinnedToCore(app_task, a->name, len > 100000 ? 32768 : 8192, a, 3, &a->task, SYS_CORE) != pdPASS) {
        app_release(a);
        msgbox(NULL, "Programs", "Not enough memory to start the program.", MB_ERROR, MB_OK);
    }
}

int os_task_mem_kb(int task_id)
{
    app_t *a = app_by_id(task_id);
    if (!a) return -1;
    return (a->mem_bytes + a->image_bytes + a->cw * a->ch + 8192 + 1023) / 1024;
}

bool os_task_kill(int task_id)
{
    app_t *a = app_by_id(task_id);
    if (!a) return false;
    // Stop it outside of an OS call, so no lock is left held.
    for (int tries = 0; tries < 200; tries++) {
        vTaskSuspend(a->task);
        if (!a->in_api) break;
        vTaskResume(a->task);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelete(a->task);
    app_release(a);
    return true;
}

// Mark windows of apps that stopped reading their events.
static void hung_check(void *arg)
{
    uint32_t now = os_uptime_ms();
    for (int i = 0; i < MAX_APPS; i++) {
        app_t *a = &s_apps[i];
        if (!a->used || !a->win) continue;
        bool hung = a->q && uxQueueMessagesWaiting(a->q) > 0 && now - a->last_poll_ms > 3000;
        bool was = a->win->flags & WIN_HUNG;
        if (hung != was) {
            if (hung) a->win->flags |= WIN_HUNG; else a->win->flags &= ~WIN_HUNG;
            wm_invalidate(a->win);
        }
        if (hung && a->closing) {
            // asked to close and not answering: offer to end it
            a->closing = false;
            char msg[120];
            snprintf(msg, sizeof(msg), "%s is not responding.\nEnd it now?", a->name);
            int id = a->id;
            if (msgbox(a->win, "Program not responding", msg, MB_WARN, MB_YESNO) == ID_YES) os_task_kill(id);
            return;
        }
    }
}

// ---------------------------------------------------------------- programs on the drive

#define MAX_DISK_APPS 16
static char s_sd_file[MAX_DISK_APPS][68], s_sd_disp[MAX_DISK_APPS][68];
static int s_sd_n;

void app_sd_rescan(void)
{
    fs_entry_t *all;
    int n = fs_list(DISK_ROOT "/Apps", &all);
    s_sd_n = 0;
    for (int i = 0; i < n && s_sd_n < MAX_DISK_APPS; i++) {
        if (all[i].is_dir || strcasecmp(fs_ext(all[i].name), "elf")) continue;
        strlcpy(s_sd_file[s_sd_n], all[i].name, 68);
        strlcpy(s_sd_disp[s_sd_n], all[i].name, 68);
        char *dot = strrchr(s_sd_disp[s_sd_n], '.');
        if (dot) *dot = 0;
        s_sd_n++;
    }
    free(all);
}

int app_sd_count(void) { return s_sd_n; }
const char *app_sd_name(int i) { return s_sd_disp[i]; }
bool app_sd_is_game(int i) { return strcasestr(s_sd_disp[i], "snake") || strcasestr(s_sd_disp[i], "doom") || strcasestr(s_sd_disp[i], "game"); }
void app_launch_sd(int i)
{
    if (i < 0 || i >= s_sd_n) return;
    char p[300];
    snprintf(p, sizeof(p), DISK_ROOT "/Apps/%s", s_sd_file[i]);
    os_run_elf(p);
}

void app_loader_start(void)
{
    boot_sub(BOOT_STAGE_APPS, 0, 2, "reading /disk/Apps");
    app_loader_init();
    app_sd_rescan();
    char detail[32];
    snprintf(detail, sizeof(detail), "%d on the card", s_sd_n);
    boot_sub(BOOT_STAGE_APPS, 1, 2, detail);
    timer_add(1000, true, hung_check, NULL);
    boot_sub(BOOT_STAGE_APPS, 2, 2, "watching for hangs");
}

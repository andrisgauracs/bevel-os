// Window manager: z-order, frames, focus, mouse and keyboard dispatch, modal loops, timers.
#include <string.h>
#include <stdlib.h>
#include "wm.h"
#include "comp.h"
#include "display.h"
#include "os.h"
#include "esp_heap_caps.h"

static window_t *s_bottom, *s_top;      // z-order
static window_t *s_dead;                // destroyed, not yet freed (linked via ->next)
static window_t *s_active;              // window with the keyboard, NULL = shell (desktop/taskbar)
static widget_t *s_capture;             // widget that got the mouse-down
static widget_t *s_pinch;               // widget under a two-finger gesture
static widget_t *s_touch_scroll;        // widget a finger is scrolling
static bool s_touch_scroll_popup;       // ... or the open drop-down list
static int s_touch_y, s_touch_rest;     // the finger's last height; pixels not yet a whole row
static const bitmap_t *s_cursor, *s_cursor_wanted;
static int s_busy;

enum { HT_NONE, HT_CLIENT, HT_TITLE, HT_CLOSE, HT_MAX, HT_MIN, HT_MENUBAR, HT_BORDER };
#define EDGE_L 1
#define EDGE_R 2
#define EDGE_T 4
#define EDGE_B 8

// mouse drag / keyboard move-size state
static struct {
    window_t *win;
    int mode;           // 0 none, 1 mouse move, 2 mouse size, 3 keyboard move, 4 keyboard size
    int edges;
    int dx, dy;         // pointer offset inside the frame (move) / start pointer (size)
    rect_t orig;
} s_drag;
static struct { window_t *win; int which; bool over; } s_tbtn; // pressed title-bar button

// double-click detection
static uint32_t s_last_down_ms;
static int s_last_down_x, s_last_down_y, s_last_down_btn, s_last_clicks;

// ---------------------------------------------------------------- timers
#define MAX_TIMERS 24
static struct { int id; int period; uint32_t due; bool repeat; timer_cb_t cb; void *arg; } s_timers[MAX_TIMERS];
static int s_timer_seq = 1;

int timer_add(int period_ms, bool repeat, timer_cb_t cb, void *arg)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!s_timers[i].id) {
            s_timers[i].id = s_timer_seq++;
            s_timers[i].period = period_ms;
            s_timers[i].due = os_uptime_ms() + period_ms;
            s_timers[i].repeat = repeat;
            s_timers[i].cb = cb;
            s_timers[i].arg = arg;
            return s_timers[i].id;
        }
    }
    return 0;
}

void timer_del(int id)
{
    if (!id) return;
    for (int i = 0; i < MAX_TIMERS; i++) if (s_timers[i].id == id) s_timers[i].id = 0;
}

static int timers_next_ms(void)
{
    int best = 1000;
    uint32_t now = os_uptime_ms();
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!s_timers[i].id) continue;
        int d = (int)(s_timers[i].due - now);
        if (d < 0) d = 0;
        if (d < best) best = d;
    }
    return best;
}

static void timers_run(void)
{
    uint32_t now = os_uptime_ms();
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!s_timers[i].id || (int)(s_timers[i].due - now) > 0) continue;
        timer_cb_t cb = s_timers[i].cb;
        void *arg = s_timers[i].arg;
        if (s_timers[i].repeat) {
            s_timers[i].due += s_timers[i].period;
            if ((int)(s_timers[i].due - now) < 0) s_timers[i].due = now + s_timers[i].period;
        } else {
            s_timers[i].id = 0;
        }
        cb(arg);
    }
}

// ---------------------------------------------------------------- clipboard
static char *s_clip;
static int s_clip_len;

void clip_set(const char *s, int len)
{
    char *n = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!n) return;
    memcpy(n, s, len);
    n[len] = 0;
    free(s_clip);
    s_clip = n;
    s_clip_len = len;
}

const char *clip_get(int *len)
{
    if (len) *len = s_clip_len;
    return s_clip ? s_clip : "";
}

// ---------------------------------------------------------------- geometry

static inline bool visible(const window_t *w) { return !(w->flags & (WIN_HIDDEN | WIN_MINIMIZED | WIN_DEAD)); }
static inline int top_h(const window_t *w) { return BORDER_W + TITLE_H + 1 + (w->menubar ? MENUBAR_H : 0); }

void wm_client_rect(const window_t *w, rect_t *out)
{
    out->x = w->r.x + BORDER_W;
    out->y = w->r.y + top_h(w);
    out->w = w->r.w - 2 * BORDER_W;
    out->h = w->r.h - top_h(w) - BORDER_W;
}
int wm_client_w(const window_t *w) { return w->r.w - 2 * BORDER_W; }
int wm_client_h(const window_t *w) { return w->r.h - top_h(w) - BORDER_W; }

static void title_rect(const window_t *w, rect_t *t)
{
    t->x = w->r.x + BORDER_W; t->y = w->r.y + BORDER_W; t->w = w->r.w - 2 * BORDER_W; t->h = TITLE_H;
}

#define TB_W 16
#define TB_H 14
// which: HT_CLOSE, HT_MAX, HT_MIN. Returns false if the window does not have that button.
static bool tbtn_rect(const window_t *w, int which, rect_t *b)
{
    rect_t t;
    title_rect(w, &t);
    int x = t.x + t.w - 2 - TB_W;
    b->y = t.y + 2; b->w = TB_W; b->h = TB_H;
    if (!(w->flags & WIN_NO_CLOSE)) {
        if (which == HT_CLOSE) { b->x = x; return true; }
        x -= TB_W + 2;
    } else if (which == HT_CLOSE) return false;
    if (w->flags & WIN_DIALOG) return false;
    if (w->flags & WIN_RESIZABLE) {
        if (which == HT_MAX) { b->x = x; return true; }
        x -= TB_W;
    } else if (which == HT_MAX) return false;
    if (which == HT_MIN) { b->x = x; return true; }
    return false;
}

static int hit_test(const window_t *w, int x, int y, int *edges)
{
    *edges = 0;
    if (!rect_has(&w->r, x, y)) return HT_NONE;
    rect_t b;
    for (int which = HT_CLOSE; which <= HT_MIN; which++) {
        if (tbtn_rect(w, which, &b) && rect_has(&b, x, y)) return which;
    }
    if ((w->flags & WIN_RESIZABLE) && !(w->flags & WIN_MAXIMIZED)) {
        int e = 0;
        const int c = 14; // corner reach
        bool l = x < w->r.x + BORDER_W, r = x >= w->r.x + w->r.w - BORDER_W;
        bool t = y < w->r.y + BORDER_W, bt = y >= w->r.y + w->r.h - BORDER_W;
        if (l || r || t || bt) {
            if (l || ((t || bt) && x < w->r.x + c)) e |= EDGE_L;
            if (r || ((t || bt) && x >= w->r.x + w->r.w - c)) e |= EDGE_R;
            if (t || ((l || r) && y < w->r.y + c)) e |= EDGE_T;
            if (bt || ((l || r) && y >= w->r.y + w->r.h - c)) e |= EDGE_B;
            *edges = e;
            return HT_BORDER;
        }
    }
    rect_t t;
    title_rect(w, &t);
    if (rect_has(&t, x, y)) return HT_TITLE;
    if (w->menubar && y >= t.y + TITLE_H + 1 && y < t.y + TITLE_H + 1 + MENUBAR_H && x >= t.x && x < t.x + t.w) return HT_MENUBAR;
    rect_t c;
    wm_client_rect(w, &c);
    if (rect_has(&c, x, y)) return HT_CLIENT;
    return HT_TITLE; // frame of a non-resizable window drags like the title
}

// ---------------------------------------------------------------- painting

static const uint16_t g_close[7] = {0xC300, 0x6600, 0x3C00, 0x1800, 0x3C00, 0x6600, 0xC300};
static const uint16_t g_min[7] = {0, 0, 0, 0, 0, 0xFC00, 0xFC00};
static const uint16_t g_max[8] = {0xFF80, 0xFF80, 0x8080, 0x8080, 0x8080, 0x8080, 0x8080, 0xFF80};
static const uint16_t g_restore[9] = {0x3F00, 0x3F00, 0x2100, 0xFD00, 0xFD00, 0x8700, 0x8400, 0x8400, 0xFC00};

static void paint_tbtn(gfx_t *g, const window_t *w, int which)
{
    rect_t b;
    if (!tbtn_rect(w, which, &b)) return;
    bool down = s_tbtn.win == w && s_tbtn.which == which && s_tbtn.over;
    gfx_fill(g, b.x, b.y, b.w, b.h, C_FACE);
    if (down) ui_draw_pressed(g, b.x, b.y, b.w, b.h); else ui_draw_raised(g, b.x, b.y, b.w, b.h);
    int ox = b.x + (down ? 1 : 0), oy = b.y + (down ? 1 : 0);
    switch (which) {
    case HT_CLOSE: gfx_glyph16(g, ox + 4, oy + 3, g_close, 8, 7, C_BLACK); break;
    case HT_MIN: gfx_glyph16(g, ox + 4, oy + 3, g_min, 6, 7, C_BLACK); break;
    case HT_MAX:
        if (w->flags & WIN_MAXIMIZED) gfx_glyph16(g, ox + 4, oy + 2, g_restore, 8, 9, C_BLACK);
        else gfx_glyph16(g, ox + 3, oy + 3, g_max, 9, 8, C_BLACK);
        break;
    }
}

static void paint_window(gfx_t *g, window_t *w)
{
    gfx_state_t st;
    gfx_save(g, &st);
    const rect_t *r = &w->r;
    if (!gfx_clip(g, r->x, r->y, r->w, r->h)) { gfx_restore(g, &st); return; }
    bool active = w == s_active;

    rect_t c;
    wm_client_rect(w, &c);
    // frame (skip when the clip is entirely inside the client area)
    bool frame = g->cx0 < c.x || g->cy0 < c.y || g->cx1 > c.x + c.w || g->cy1 > c.y + c.h;
    gfx_fill(g, r->x, r->y, r->w, r->h, C_FACE);
    if (frame) {
        gfx_bevel(g, r->x, r->y, r->w, r->h, C_FACE, C_DKSHADOW);
        gfx_bevel(g, r->x + 1, r->y + 1, r->w - 2, r->h - 2, C_HILIGHT, C_SHADOW);
        rect_t t;
        title_rect(w, &t);
        gfx_fill(g, t.x, t.y, t.w, t.h, active ? C_TITLE : C_TITLE_OFF);
        int tx = t.x + 3;
        if (w->icon) { gfx_bitmap(g, t.x + 1, t.y + 1, w->icon); tx = t.x + 20; }
        gfx_state_t ts;
        gfx_save(g, &ts);
        rect_t last;
        int right = t.x + t.w - 4;
        for (int which = HT_CLOSE; which <= HT_MIN; which++) if (tbtn_rect(w, which, &last)) right = last.x - 3;
        if (gfx_clip(g, tx, t.y, right - tx, t.h)) {
            int adv = gfx_text(g, tx, t.y + 3, w->title, -1, FONT_UI_BOLD, active ? C_TITLE_TEXT : C_TITLE_OFF_TEXT);
            if (w->flags & WIN_HUNG) gfx_text(g, tx + adv + 4, t.y + 3, "(not responding)", -1, FONT_UI_BOLD, active ? C_TITLE_TEXT : C_TITLE_OFF_TEXT);
        }
        gfx_restore(g, &ts);
        paint_tbtn(g, w, HT_CLOSE);
        paint_tbtn(g, w, HT_MAX);
        paint_tbtn(g, w, HT_MIN);
        if (w->menubar) {
            rect_t bar = {t.x, t.y + TITLE_H + 1, t.w, MENUBAR_H};
            menubar_paint(w, g, &bar);
        }
    }
    // client
    if (gfx_clip(g, c.x, c.y, c.w, c.h)) {
        gfx_translate(g, c.x, c.y);
        gfx_state_t cs;
        gfx_save(g, &cs);
        for (widget_t *wd = w->widgets; wd; wd = wd->next) {
            if (wd->flags & WF_HIDDEN) continue;
            if (!gfx_visible(g, wd->r.x, wd->r.y, wd->r.w, wd->r.h)) continue;
            if (gfx_clip(g, wd->r.x, wd->r.y, wd->r.w, wd->r.h)) {
                gfx_translate(g, wd->r.x, wd->r.y);
                wd->cls->paint(wd, g);
            }
            gfx_restore(g, &cs);
        }
    }
    gfx_restore(g, &st);
}

static void wm_paint(gfx_t *g, const rect_t *area)
{
    // Start at the topmost window that covers the whole area; nothing below it can show.
    window_t *start = NULL;
    for (window_t *w = s_top; w; w = w->prev) {
        if (!visible(w)) continue;
        if (area->x >= w->r.x && area->y >= w->r.y && area->x + area->w <= w->r.x + w->r.w &&
            area->y + area->h <= w->r.y + w->r.h) { start = w; break; }
    }
    gfx_state_t st;
    gfx_save(g, &st);
    if (!start) {
        if (area->y < DESK_H) shell_paint_desktop(g, area);
        gfx_restore(g, &st);
        start = s_bottom;
    }
    for (window_t *w = start; w; w = w->next) {
        if (visible(w) && rect_overlap(&w->r, area)) paint_window(g, w);
    }
    if (area->y + area->h > DESK_H) { shell_paint_taskbar(g, area); gfx_restore(g, &st); }
    menu_paint(g, area);
    gfx_restore(g, &st);
    ui_popup_paint(g, area);
    gfx_restore(g, &st);
    shell_paint_overlay(g, area);
    gfx_restore(g, &st);
}

void wm_invalidate(window_t *w)
{
    if (visible(w)) comp_invalidate_rect(&w->r);
}

static void invalidate_title(window_t *w)
{
    if (!w || !visible(w)) return;
    comp_invalidate(w->r.x, w->r.y, w->r.w, top_h(w));
}

// ---------------------------------------------------------------- z-order

static void unlink_win(window_t *w)
{
    if (w->prev) w->prev->next = w->next; else if (s_bottom == w) s_bottom = w->next;
    if (w->next) w->next->prev = w->prev; else if (s_top == w) s_top = w->prev;
    w->prev = w->next = NULL;
}

static void link_top(window_t *w)
{
    w->prev = s_top; w->next = NULL;
    if (s_top) s_top->next = w; else s_bottom = w;
    s_top = w;
}

window_t *wm_active(void) { return s_active; }
window_t *wm_top(void) { return s_top; }

bool wm_valid(window_t *w)
{
    for (window_t *i = s_bottom; i; i = i->next) if (i == w) return true;
    return false;
}

static window_t *modal_child(window_t *w)
{
    for (window_t *d = s_top; d; d = d->prev) {
        if (d->owner == w && !(d->flags & (WIN_DEAD | WIN_HIDDEN))) return d;
    }
    return NULL;
}

static void set_active(window_t *w)
{
    if (s_active == w) return;
    window_t *old = s_active;
    s_active = w;
    if (old) {
        invalidate_title(old);
        if (old->focus) ui_invalidate(old->focus);
        if (old->on_activate) old->on_activate(old, false);
    }
    if (w) {
        invalidate_title(w);
        if (w->focus) ui_invalidate(w->focus);
        if (w->on_activate) w->on_activate(w, true);
    }
    shell_windows_changed();
}

static void raise(window_t *w)
{
    if (w->owner && wm_valid(w->owner)) raise(w->owner);
    if (s_top != w) {
        unlink_win(w);
        link_top(w);
        wm_invalidate(w);
    }
}

void wm_activate(window_t *w)
{
    if (menu_is_open()) menu_close_all();
    ui_popup_close();
    if (!w) { set_active(NULL); return; }
    if (!wm_valid(w) || (w->flags & (WIN_DEAD | WIN_HIDDEN))) return;
    for (window_t *d; (d = modal_child(w)) != NULL;) w = d;
    if (w->flags & WIN_MINIMIZED) {
        w->flags &= ~WIN_MINIMIZED;
        wm_invalidate(w);
    }
    raise(w);
    set_active(w);
}

static void activate_next(void)
{
    for (window_t *w = s_top; w; w = w->prev) {
        if (visible(w)) { wm_activate(w); return; }
    }
    set_active(NULL);
    shell_focus_desktop();
}

// ---------------------------------------------------------------- lifecycle

window_t *wm_create(const char *title, int cw, int ch, uint16_t flags)
{
    window_t *w = heap_caps_calloc(1, sizeof(window_t), MALLOC_CAP_INTERNAL);
    if (!w) return NULL;
    strlcpy(w->title, title, sizeof(w->title));
    w->flags = flags | WIN_HIDDEN;
    w->min_w = 140; w->min_h = 80;
    static int cascade;
    w->r.w = cw + 2 * BORDER_W;
    w->r.h = ch + top_h(w) + BORDER_W;
    if (flags & WIN_DIALOG) {
        w->r.x = (SCREEN_W - w->r.w) / 2;
        w->r.y = (DESK_H - w->r.h) / 2;
    } else {
        w->r.x = 30 + (cascade % 6) * 22;
        w->r.y = 16 + (cascade % 6) * 22;
        cascade++;
        if (w->r.x + w->r.w > SCREEN_W) w->r.x = SCREEN_W - w->r.w;
        if (w->r.y + w->r.h > DESK_H) w->r.y = DESK_H - w->r.h;
    }
    if (w->r.x < 0) w->r.x = 0;
    if (w->r.y < 0) w->r.y = 0;
    link_top(w);
    return w;
}

void wm_set_menubar(window_t *w, const menu_item_t *bar)
{
    if (!w->menubar && bar) w->r.h += MENUBAR_H;
    w->menubar = bar;
}

void wm_show(window_t *w)
{
    // a menu bar set after wm_create changes the frame height: keep the client size
    w->flags &= ~WIN_HIDDEN;
    if (w->on_resize) w->on_resize(w);
    if (!w->focus) {
        for (widget_t *wd = w->widgets; wd; wd = wd->next) {
            if ((wd->flags & WF_TABSTOP) && !(wd->flags & (WF_HIDDEN | WF_DISABLED))) { w->focus = wd; break; }
        }
    }
    wm_invalidate(w);
    wm_activate(w);
    shell_windows_changed();
}

void wm_destroy(window_t *w)
{
    if (!w || (w->flags & WIN_DEAD)) return;
    if (menu_is_open()) menu_close_all();
    ui_popup_close();
    wm_invalidate(w);
    w->flags |= WIN_DEAD;
    // cancel dialogs that are modal to this window
    for (window_t *d = s_top; d; d = d->prev) {
        if (d->owner == w && !(d->flags & WIN_DEAD)) { d->dlg_done = true; d->dlg_result = ID_CANCEL; wm_invalidate(d); d->flags |= WIN_HIDDEN; }
    }
    if (s_capture && s_capture->win == w) s_capture = NULL;
    if (s_pinch && s_pinch->win == w) s_pinch = NULL;
    if (s_touch_scroll && s_touch_scroll->win == w) s_touch_scroll = NULL;
    if (s_drag.win == w) s_drag.mode = 0, s_drag.win = NULL;
    if (s_tbtn.win == w) s_tbtn.win = NULL;
    unlink_win(w);
    w->next = s_dead;
    s_dead = w;
    if (s_active == w) {
        s_active = NULL;
        if (w->owner && wm_valid(w->owner) && !(w->owner->flags & WIN_DEAD) && !w->dlg_done) wm_activate(w->owner);
        else if (!w->owner || !wm_valid(w->owner)) activate_next();
    }
    shell_windows_changed();
}

// Free windows destroyed earlier, once none of their code can still be on the stack.
static void reap(void)
{
    window_t **pp = &s_dead;
    while (*pp) {
        window_t *w = *pp;
        if (w->modal_children > 0) { pp = &w->next; continue; }
        *pp = w->next;
        if (w->on_destroy) w->on_destroy(w);
        while (w->widgets) ui_delete(w->widgets);
        free(w);
    }
}

void wm_close(window_t *w)
{
    if (!w || (w->flags & WIN_DEAD)) return;
    if (w->modal_children > 0) { wm_activate(w); return; }
    if (w->on_close && !w->on_close(w)) return;
    if (w->flags & WIN_DEAD) return;
    wm_destroy(w);
}

int wm_run_modal(window_t *dlg)
{
    window_t *owner = dlg->owner;
    if (owner) owner->modal_children++;
    dlg->dlg_done = false;
    dlg->dlg_result = ID_CANCEL;
    wm_show(dlg);
    while (!dlg->dlg_done && !(dlg->flags & WIN_DEAD)) wm_pump(100);
    int res = dlg->dlg_result;
    if (owner) owner->modal_children--;
    bool owner_ok = owner && wm_valid(owner) && !(owner->flags & WIN_DEAD);
    dlg->dlg_done = true;
    wm_destroy(dlg);
    if (owner_ok) wm_activate(owner);
    else if (!s_active) activate_next();
    return res;
}

void wm_end_modal(window_t *dlg, int result)
{
    dlg->dlg_result = result;
    dlg->dlg_done = true;
}

// ---------------------------------------------------------------- move / size / state

static void set_frame(window_t *w, int x, int y, int fw, int fh)
{
    if (fw < w->min_w) fw = w->min_w;
    if (fh < w->min_h) fh = w->min_h;
    if (fw > SCREEN_W + 2 * BORDER_W) fw = SCREEN_W + 2 * BORDER_W;
    if (fh > SCREEN_H) fh = SCREEN_H;
    if (x == w->r.x && y == w->r.y && fw == w->r.w && fh == w->r.h) return;
    bool resized = fw != w->r.w || fh != w->r.h;
    rect_t old = w->r, u;
    w->r.x = x; w->r.y = y; w->r.w = fw; w->r.h = fh;
    if (visible(w)) {
        rect_union(&u, &old, &w->r);
        // one rectangle when the move is small (the usual drag step), two when it jumped
        if (u.w * u.h <= old.w * old.h + w->r.w * w->r.h) comp_invalidate_rect(&u);
        else { comp_invalidate_rect(&old); comp_invalidate_rect(&w->r); }
    }
    if (resized && w->on_resize) w->on_resize(w);
}

void wm_move(window_t *w, int x, int y)
{
    // keep a grabbable part of the title bar on screen
    if (x > SCREEN_W - 40) x = SCREEN_W - 40;
    if (x < 40 - w->r.w) x = 40 - w->r.w;
    if (y < 0) y = 0;
    if (y > DESK_H - TITLE_H - BORDER_W) y = DESK_H - TITLE_H - BORDER_W;
    set_frame(w, x, y, w->r.w, w->r.h);
}

void wm_resize(window_t *w, int fw, int fh) { set_frame(w, w->r.x, w->r.y, fw, fh); }

void wm_minimize(window_t *w)
{
    if (w->flags & (WIN_DIALOG | WIN_MINIMIZED)) return;
    wm_invalidate(w);
    w->flags |= WIN_MINIMIZED;
    if (s_active == w) { s_active = NULL; if (w->on_activate) w->on_activate(w, false); activate_next(); }
    shell_windows_changed();
}

void wm_maximize(window_t *w, bool on)
{
    if (!(w->flags & WIN_RESIZABLE)) return;
    if (on && !(w->flags & WIN_MAXIMIZED)) {
        w->restore = w->r;
        w->flags |= WIN_MAXIMIZED;
        set_frame(w, -BORDER_W, -BORDER_W, SCREEN_W + 2 * BORDER_W, DESK_H + 2 * BORDER_W);
    } else if (!on && (w->flags & WIN_MAXIMIZED)) {
        w->flags &= ~WIN_MAXIMIZED;
        set_frame(w, w->restore.x, w->restore.y, w->restore.w, w->restore.h);
    }
    wm_invalidate(w);
}

void wm_set_title(window_t *w, const char *title)
{
    if (!strcmp(w->title, title)) return;
    strlcpy(w->title, title, sizeof(w->title));
    invalidate_title(w);
    shell_windows_changed();
}

void wm_start_move(window_t *w, bool size)
{
    if (!w || (w->flags & WIN_MAXIMIZED)) return;
    if (size && !(w->flags & WIN_RESIZABLE)) return;
    s_drag.win = w;
    s_drag.mode = size ? 4 : 3;
    s_drag.orig = w->r;
    wm_set_cursor(size ? &cursor_size_nwse : &cursor_move);
    // put the pointer where the action is, as a visible hint
    if (size) input_mouse_abs(w->r.x + w->r.w - 2, w->r.y + w->r.h - 2);
    else input_mouse_abs(w->r.x + w->r.w / 2, w->r.y + BORDER_W + TITLE_H / 2);
}

static bool kb_move_key(const event_t *e)
{
    window_t *w = s_drag.win;
    int step = (e->mods & MOD_CTRL) ? 1 : 8;
    int dx = 0, dy = 0;
    switch (e->key) {
    case K_LEFT: dx = -step; break;
    case K_RIGHT: dx = step; break;
    case K_UP: dy = -step; break;
    case K_DOWN: dy = step; break;
    case K_ENTER: case K_KP_ENTER:
        s_drag.mode = 0; s_drag.win = NULL; wm_set_cursor(&cursor_arrow);
        return true;
    case K_ESC:
        set_frame(w, s_drag.orig.x, s_drag.orig.y, s_drag.orig.w, s_drag.orig.h);
        s_drag.mode = 0; s_drag.win = NULL; wm_set_cursor(&cursor_arrow);
        return true;
    default:
        return true; // swallow everything else while in this mode
    }
    if (s_drag.mode == 3) {
        wm_move(w, w->r.x + dx, w->r.y + dy);
        input_mouse_abs(w->r.x + w->r.w / 2, w->r.y + BORDER_W + TITLE_H / 2);
    } else {
        wm_resize(w, w->r.w + dx, w->r.h + dy);
        input_mouse_abs(w->r.x + w->r.w - 2, w->r.y + w->r.h - 2);
    }
    return true;
}

// ---------------------------------------------------------------- window menu (Alt+Space)

enum { WMC_RESTORE = 1, WMC_MOVE, WMC_SIZE, WMC_MIN, WMC_MAX, WMC_CLOSE };
static menu_item_t s_winmenu[8];

static void winmenu_cb(void *ctx, int cmd)
{
    window_t *w = ctx;
    if (!wm_valid(w)) return;
    switch (cmd) {
    case WMC_RESTORE: if (w->flags & WIN_MINIMIZED) wm_activate(w); else wm_maximize(w, false); break;
    case WMC_MOVE: wm_start_move(w, false); break;
    case WMC_SIZE: wm_start_move(w, true); break;
    case WMC_MIN: wm_minimize(w); break;
    case WMC_MAX: wm_maximize(w, true); break;
    case WMC_CLOSE: wm_close(w); break;
    }
}

void wm_window_menu(window_t *w)
{
    if (!w) return;
    bool dlg = w->flags & WIN_DIALOG, rs = w->flags & WIN_RESIZABLE, mx = w->flags & WIN_MAXIMIZED;
    menu_item_t items[] = {
        {"&Restore", WMC_RESTORE, NULL, NULL, mx ? 0 : MI_DISABLED},
        {"&Move", WMC_MOVE, NULL, NULL, mx ? MI_DISABLED : 0},
        {"&Size", WMC_SIZE, NULL, NULL, (rs && !mx) ? 0 : MI_DISABLED},
        {"Mi&nimise", WMC_MIN, NULL, NULL, dlg ? MI_DISABLED : 0},
        {"Ma&ximise", WMC_MAX, NULL, NULL, (rs && !mx) ? 0 : MI_DISABLED},
        {"-", 0, NULL, NULL, 0},
        {"&Close\tAlt+F4", WMC_CLOSE, NULL, NULL, (w->flags & WIN_NO_CLOSE) ? MI_DISABLED : 0},
        {NULL, 0, NULL, NULL, 0},
    };
    memcpy(s_winmenu, items, sizeof(items));
    menu_popup(s_winmenu, w->r.x + BORDER_W, w->r.y + BORDER_W + TITLE_H, false, winmenu_cb, w);
}

// ---------------------------------------------------------------- focus helpers

static bool focusable(const widget_t *w)
{
    return (w->flags & WF_TABSTOP) && !(w->flags & (WF_HIDDEN | WF_DISABLED));
}

static void focus_step(window_t *win, bool back)
{
    widget_t *start = win->focus, *w = start;
    for (int guard = 0; guard < 1000; guard++) {
        if (!w) w = back ? win->widgets_tail : win->widgets;
        else w = back ? w->prev : w->next;
        if (!w) { if (!start) return; continue; }
        if (w == start) return;
        if (focusable(w)) { ui_focus(w); return; }
        if (!start && guard > 500) return;
    }
}

static void send_space(widget_t *w)
{
    event_t e = {.type = EV_KEY, .key = K_SPACE, .ch = ' ', .down = 1};
    if (w->cls->key) w->cls->key(w, &e);
}

static bool mnemonic(window_t *win, char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    for (widget_t *w = win->widgets; w; w = w->next) {
        if (!w->text || (w->flags & (WF_HIDDEN | WF_DISABLED))) continue;
        if (gfx_label_mnemonic(w->text) != ch) continue;
        if (focusable(w)) { ui_focus(w); send_space(w); return true; }
        // a label: focus the next control
        for (widget_t *n = w->next; n; n = n->next) {
            if (focusable(n)) { ui_focus(n); return true; }
        }
    }
    return false;
}

// ---------------------------------------------------------------- keyboard

static bool s_alt_clean;
bool wm_alt_tapped(const event_t *e)
{
    return !e->down && (e->key == K_LALT || e->key == K_RALT) && s_alt_clean;
}

static void handle_key(const event_t *e)
{
    bool is_alt = e->key == K_LALT || e->key == K_RALT;
    if (e->down) s_alt_clean = is_alt && !(e->mods & (MOD_CTRL | MOD_SHIFT | MOD_GUI));
    if (shell_key(e)) { if (!is_alt) s_alt_clean = false; return; }
    if (menu_is_open()) { if (e->down) menu_key(e); s_alt_clean = false; return; }
    if (ui_popup_is_open() && ui_popup_key(e)) { s_alt_clean = false; return; }
    if (s_drag.mode >= 3) { if (e->down) kb_move_key(e); return; }
    if (s_drag.mode) return;
    if (shell_has_focus() || !s_active) { if (e->down) shell_focus_key(e); return; }
    window_t *w = s_active;
    if (menubar_key(w, e)) return;
    widget_t *f = w->focus;
    if (!e->down) {
        // key releases only matter to custom canvases (games)
        if (f && (f->flags & WF_WANT_KEYUP) && f->cls->key) f->cls->key(f, e);
        return;
    }
    if (w->on_key && w->on_key(w, e)) return;
    if (!wm_valid(w) || s_active != w) return;
    f = w->focus;
    uint8_t m = e->mods & (MOD_CTRL | MOD_ALT | MOD_GUI);
    if ((e->mods & MOD_ALT) && e->ch && e->ch != ' ') { if (mnemonic(w, e->ch)) return; }
    if (e->key == K_TAB && !m && !(f && (f->flags & WF_WANT_TAB))) { focus_step(w, e->mods & MOD_SHIFT); return; }
    if ((e->key == K_ENTER || e->key == K_KP_ENTER) && !m && !(f && (f->flags & WF_WANT_ENTER))) {
        if (f && f->cls->name[0] == 'b' && !strcmp(f->cls->name, "button")) { ui_notify(f, UI_CLICK); return; }
        if (w->def_btn && !(w->def_btn->flags & WF_DISABLED)) { ui_notify(w->def_btn, UI_CLICK); return; }
    }
    if (e->key == K_ESC && !m && !(f && (f->flags & WF_WANT_ESC))) {
        if (w->cancel_btn) { ui_notify(w->cancel_btn, UI_CLICK); return; }
        if (w->flags & WIN_DIALOG) { wm_close(w); return; }
    }
    if (f && f->cls->key && !(f->flags & WF_DISABLED) && f->cls->key(f, e)) return;
    if (!wm_valid(w) || (w->flags & WIN_DEAD)) return;
    if (e->ch && e->ch > ' ' && !m && !(f && (f->flags & WF_WANT_CHARS))) { if (mnemonic(w, e->ch)) return; }
    if (!m && !(f && (f->flags & WF_WANT_ARROWS))) {
        if (e->key == K_RIGHT || e->key == K_DOWN) focus_step(w, false);
        else if (e->key == K_LEFT || e->key == K_UP) focus_step(w, true);
    }
}

// ---------------------------------------------------------------- mouse

void wm_set_cursor(const bitmap_t *c)
{
    s_cursor_wanted = c;
    if (s_busy) c = &cursor_busy;
    if (c == s_cursor) return;
    s_cursor = c;
    disp_cursor_shape(c->px, c->w, c->h, c->hot_x, c->hot_y);
}

void wm_busy(bool on)
{
    s_busy += on ? 1 : -1;
    if (s_busy < 0) s_busy = 0;
    // hidden before the shape changes back, shown after it changed: touch never sees the arrow flash
    if (!s_busy) input_pointer_busy(false);
    wm_set_cursor(s_cursor_wanted ? s_cursor_wanted : &cursor_arrow);
    if (s_busy) input_pointer_busy(true);
}

static const bitmap_t *edge_cursor(int edges)
{
    if (edges == (EDGE_L | EDGE_T) || edges == (EDGE_R | EDGE_B)) return &cursor_size_nwse;
    if (edges == EDGE_L || edges == EDGE_R) return &cursor_size_we;
    if (edges == EDGE_T || edges == EDGE_B) return &cursor_size_ns;
    return &cursor_move; // the other diagonal has no dedicated shape
}

static widget_t *widget_at(window_t *w, int x, int y, int *lx, int *ly)
{
    rect_t c;
    wm_client_rect(w, &c);
    int cx = x - c.x, cy = y - c.y;
    widget_t *hit = NULL;
    for (widget_t *wd = w->widgets; wd; wd = wd->next) {
        if (wd->flags & WF_HIDDEN) continue;
        if (rect_has(&wd->r, cx, cy)) hit = wd; // later widgets are on top
    }
    if (hit) { *lx = cx - hit->r.x; *ly = cy - hit->r.y; }
    return hit;
}

static window_t *window_at(int x, int y)
{
    for (window_t *w = s_top; w; w = w->prev) {
        if (visible(w) && rect_has(&w->r, x, y)) return w;
    }
    return NULL;
}

static void drag_update(int x, int y)
{
    window_t *w = s_drag.win;
    if (s_drag.mode == 1) {
        wm_move(w, x - s_drag.dx, y - s_drag.dy);
    } else if (s_drag.mode == 2) {
        int ddx = x - s_drag.dx, ddy = y - s_drag.dy;
        rect_t r = s_drag.orig;
        if (s_drag.edges & EDGE_L) { int nw = r.w - ddx; if (nw < w->min_w) nw = w->min_w; r.x = r.x + r.w - nw; r.w = nw; }
        if (s_drag.edges & EDGE_R) r.w += ddx;
        if (s_drag.edges & EDGE_T) { int nh = r.h - ddy; if (nh < w->min_h) nh = w->min_h; if (r.y + r.h - nh < 0) nh = r.y + r.h; r.y = r.y + r.h - nh; r.h = nh; }
        if (s_drag.edges & EDGE_B) r.h += ddy;
        set_frame(w, r.x, r.y, r.w, r.h);
    }
}

static void handle_mouse(int type, int x, int y, int btn)
{
    int clicks = 1;
    if (type == EV_MOUSE_DOWN) {
        s_alt_clean = false;
        uint32_t now = os_uptime_ms();
        int dx = x - s_last_down_x, dy = y - s_last_down_y;
        int tol = input_touching() ? 24 : 5;   // two taps of a finger never land on the same pixels
        if (btn == s_last_down_btn && now - s_last_down_ms < (input_touching() ? 600 : 450) && dx > -tol && dx < tol && dy > -tol && dy < tol && s_last_clicks == 1) clicks = 2;
        s_last_down_ms = now; s_last_down_x = x; s_last_down_y = y; s_last_down_btn = btn; s_last_clicks = clicks;
    }

    if (s_drag.mode >= 3) {
        // keyboard move/size: a click ends it
        if (type == EV_MOUSE_DOWN) { s_drag.mode = 0; s_drag.win = NULL; wm_set_cursor(&cursor_arrow); }
        return;
    }
    if (s_drag.mode) {
        if (type == EV_MOUSE_MOVE) drag_update(x, y);
        else if (type == EV_MOUSE_UP && btn == BTN_LEFT) { s_drag.mode = 0; s_drag.win = NULL; }
        return;
    }
    if (s_tbtn.win) {
        rect_t b;
        window_t *w = s_tbtn.win;
        bool over = tbtn_rect(w, s_tbtn.which, &b) && rect_has(&b, x, y);
        if (over != s_tbtn.over) { s_tbtn.over = over; comp_invalidate_rect(&b); }
        if (type == EV_MOUSE_UP && btn == BTN_LEFT) {
            int which = s_tbtn.which;
            s_tbtn.win = NULL;
            comp_invalidate_rect(&b);
            if (over) {
                if (which == HT_CLOSE) wm_close(w);
                else if (which == HT_MIN) wm_minimize(w);
                else if (which == HT_MAX) wm_maximize(w, !(w->flags & WIN_MAXIMIZED));
            }
        }
        return;
    }
    if (s_capture) {
        widget_t *wd = s_capture;
        rect_t c;
        wm_client_rect(wd->win, &c);
        if (type == EV_MOUSE_UP && !input_buttons()) s_capture = NULL;
        if (wd->cls->mouse && type != EV_WHEEL) wd->cls->mouse(wd, type, x - c.x - wd->r.x, y - c.y - wd->r.y, btn, clicks);
        return;
    }
    if (shell_volume_mouse(type, x, y, btn)) return;
    if (menu_is_open() && menu_mouse(type, x, y, btn)) return;
    if (ui_popup_is_open() && ui_popup_mouse(type, x, y, btn)) return;

    if (type == EV_WHEEL) {
        window_t *w = window_at(x, y);
        if (!w) w = s_active;
        if (!w || w->modal_children) return;
        int lx, ly;
        widget_t *wd = widget_at(w, x, y, &lx, &ly);
        if (!wd || !wd->cls->mouse) wd = w->focus, lx = ly = 0;
        if (wd && wd->cls->mouse) wd->cls->mouse(wd, EV_WHEEL, lx, ly, btn, 0);
        return;
    }

    if (y >= DESK_H) {
        if (type == EV_MOUSE_MOVE) wm_set_cursor(&cursor_arrow);
        shell_mouse(type, x, y, btn, clicks);
        return;
    }
    window_t *w = window_at(x, y);
    if (!w) {
        if (type == EV_MOUSE_MOVE) wm_set_cursor(&cursor_arrow);
        shell_mouse(type, x, y, btn, clicks);
        return;
    }
    int edges;
    int ht = hit_test(w, x, y, &edges);

    if (type == EV_MOUSE_MOVE) {
        const bitmap_t *cur = &cursor_arrow;
        if (w->modal_children == 0) {
            if (ht == HT_BORDER) cur = edge_cursor(edges);
            else if (ht == HT_CLIENT) {
                int lx, ly;
                widget_t *wd = widget_at(w, x, y, &lx, &ly);
                if (wd && (wd->flags & WF_IBEAM) && !(wd->flags & WF_DISABLED)) cur = &cursor_ibeam;
                if (wd && wd->cls->mouse && !(wd->flags & WF_DISABLED)) wd->cls->mouse(wd, EV_MOUSE_MOVE, lx, ly, 0, 0);
            }
        }
        wm_set_cursor(cur);
        return;
    }
    if (type != EV_MOUSE_DOWN) return;

    if (w->modal_children) { wm_activate(w); return; } // goes to its dialog
    if (w != s_active || shell_has_focus()) wm_activate(w);
    switch (ht) {
    case HT_CLOSE: case HT_MAX: case HT_MIN:
        if (btn == BTN_LEFT) {
            rect_t b;
            tbtn_rect(w, ht, &b);
            s_tbtn.win = w; s_tbtn.which = ht; s_tbtn.over = true;
            comp_invalidate_rect(&b);
        }
        break;
    case HT_BORDER:
        if (btn == BTN_LEFT) { s_drag.win = w; s_drag.mode = 2; s_drag.edges = edges; s_drag.dx = x; s_drag.dy = y; s_drag.orig = w->r; }
        break;
    case HT_TITLE:
        if (btn == BTN_RIGHT) { wm_window_menu(w); break; }
        if (btn != BTN_LEFT) break;
        if (clicks == 2 && (w->flags & WIN_RESIZABLE)) { wm_maximize(w, !(w->flags & WIN_MAXIMIZED)); break; }
        if (!(w->flags & WIN_MAXIMIZED)) { s_drag.win = w; s_drag.mode = 1; s_drag.dx = x - w->r.x; s_drag.dy = y - w->r.y; }
        break;
    case HT_MENUBAR:
        if (btn == BTN_LEFT) menubar_click(w, x, y);
        break;
    case HT_CLIENT: {
        int lx, ly;
        widget_t *wd = widget_at(w, x, y, &lx, &ly);
        if (!wd || (wd->flags & WF_DISABLED)) break;
        if (focusable(wd)) ui_focus(wd);
        s_capture = wd;
        if (wd->cls->mouse) wd->cls->mouse(wd, EV_MOUSE_DOWN, lx, ly, btn, clicks);
        break;
    }
    }
}

void ui_capture(widget_t *w) { s_capture = w; }

// A pinch goes to the widget under the fingers when it began, if that widget asked for pinches.
static void handle_pinch(const event_t *e)
{
    if (e->button == PINCH_BEGIN) {
        s_pinch = NULL;
        if (menu_is_open() || ui_popup_is_open() || s_drag.mode || s_tbtn.win || s_capture || e->y >= DESK_H) return;
        window_t *w = window_at(e->x, e->y);
        int edges, lx, ly;
        if (!w || w->modal_children || hit_test(w, e->x, e->y, &edges) != HT_CLIENT) return;
        widget_t *wd = widget_at(w, e->x, e->y, &lx, &ly);
        if (!wd || !(wd->flags & WF_WANT_PINCH) || (wd->flags & WF_DISABLED)) return;
        if (w != s_active || shell_has_focus()) wm_activate(w);
        if (focusable(wd)) ui_focus(wd);
        s_pinch = wd;
    }
    widget_t *wd = s_pinch;
    if (!wd) return;
    rect_t c;
    wm_client_rect(wd->win, &c);
    wd->cls->mouse(wd, EV_PINCH, e->x - c.x - wd->r.x, e->y - c.y - wd->r.y, e->scale, e->button);
    if (e->button == PINCH_END) s_pinch = NULL;
}

int ui_touch_steps(int dy, int step)
{
    s_touch_rest += dy;
    int n = s_touch_rest / step;
    s_touch_rest -= n * step;
    return n;
}

// Is what the finger started on something it scrolls? Scroll bars keep working as bars.
static bool touch_scroll_begin(int x, int y)
{
    s_touch_scroll = NULL;
    s_touch_scroll_popup = false;
    s_touch_y = y;
    s_touch_rest = 0;
    if (ui_popup_is_open()) return s_touch_scroll_popup = ui_popup_scrolls_at(x, y);
    if (menu_is_open() || s_drag.mode || s_tbtn.win || s_capture || y >= DESK_H) return false;
    window_t *w = window_at(x, y);
    int edges, lx, ly;
    if (!w || w->modal_children || hit_test(w, x, y, &edges) != HT_CLIENT) return false;
    widget_t *wd = widget_at(w, x, y, &lx, &ly);
    if (!wd || !(wd->flags & WF_TOUCH_SCROLL) || (wd->flags & WF_DISABLED) || lx >= wd->r.w - 2 - SCROLL_W) return false;
    if (w != s_active || shell_has_focus()) wm_activate(w);
    if (focusable(wd)) ui_focus(wd);
    s_touch_scroll = wd;
    input_touch_hide_pointer();
    return true;
}

// One finger, decided here because only the window manager knows what is under it. A drag on
// anything that does not scroll is the left button, as it always was.
static void handle_touch(const event_t *e)
{
    int x = e->x, y = e->y;
    switch (e->button) {
    case TOUCH_TAP: input_touch(x, y, true); input_touch(x, y, false); break;
    case TOUCH_LONG: input_touch_right_click(x, y); break;
    case TOUCH_DRAG_BEGIN:
        if (!touch_scroll_begin(x, y)) input_touch(x, y, true);
        break;
    case TOUCH_DRAG_MOVE: {
        input_touch_latest(&x, &y);
        int dy = y - s_touch_y;
        if (s_touch_scroll_popup) { s_touch_y = y; ui_popup_touch_scroll(dy); }
        else if (s_touch_scroll) {
            widget_t *wd = s_touch_scroll;
            rect_t c;
            wm_client_rect(wd->win, &c);
            s_touch_y = y;
            if (dy) wd->cls->mouse(wd, EV_TOUCH_SCROLL, x - c.x - wd->r.x, y - c.y - wd->r.y, dy, 0);
        } else input_touch(x, y, true);
        break;
    }
    case TOUCH_DRAG_END:
        if (!s_touch_scroll && !s_touch_scroll_popup) input_touch(x, y, false);
        s_touch_scroll = NULL;
        s_touch_scroll_popup = false;
        break;
    }
}

// ---------------------------------------------------------------- pump

static uint32_t s_fps, s_fps_count, s_fps_t0;
uint32_t wm_fps(void) { return s_fps; }

static void dispatch(const event_t *e)
{
    switch (e->type) {
    case EV_KEY: handle_key(e); break;
    case EV_MOUSE_MOVE: {
        int x, y;
        input_mouse_pos(&x, &y);
        handle_mouse(EV_MOUSE_MOVE, x, y, 0);
        break;
    }
    case EV_MOUSE_DOWN: case EV_MOUSE_UP: handle_mouse(e->type, e->x, e->y, e->button); break;
    case EV_WHEEL: {
        int x, y;
        input_mouse_pos(&x, &y);
        handle_mouse(EV_WHEEL, x, y, e->x);
        break;
    }
    case EV_PINCH: handle_pinch(e); break;
    case EV_TOUCH: handle_touch(e); break;
    case EV_CALL: if (e->fn) e->fn(e->arg); break;
    }
}

void wm_pump(int max_wait_ms)
{
    reap();
    int t = timers_next_ms();
    if (t < max_wait_ms) max_wait_ms = t;
    int rep = input_repeat_poll();
    if (rep >= 0 && rep < max_wait_ms) max_wait_ms = rep;
    if (comp_dirty()) max_wait_ms = 0;
    event_t e;
    if (ev_wait(&e, max_wait_ms)) {
        int n = 0;
        do { dispatch(&e); } while (++n < 24 && ev_wait(&e, 0));
    }
    timers_run();
    if (comp_flush()) s_fps_count++;
    uint32_t now = os_uptime_ms();
    if (now - s_fps_t0 >= 1000) { s_fps = s_fps_count * 1000 / (now - s_fps_t0); s_fps_count = 0; s_fps_t0 = now; }
}

void wm_run(void)
{
    for (;;) wm_pump(1000);
}

void wm_init(void)
{
    comp_init(wm_paint);
    s_cursor = NULL;
    wm_set_cursor(&cursor_arrow);
    int x, y;
    input_mouse_pos(&x, &y);
    disp_cursor_move(x, y);
    shell_init();
    comp_invalidate_all();
}

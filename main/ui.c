// Widget toolkit: buttons, check boxes, radio buttons, labels, text fields, sliders, list views,
// canvases and the scrollbar helper. Every widget works with the keyboard and the mouse.
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "wm.h"
#include "comp.h"
#include "os.h"
#include "esp_heap_caps.h"

// ---------------------------------------------------------------- theme helpers

void ui_draw_raised(gfx_t *g, int x, int y, int w, int h)
{
    gfx_bevel(g, x, y, w, h, C_HILIGHT, C_DKSHADOW);
    gfx_bevel(g, x + 1, y + 1, w - 2, h - 2, C_FACE, C_SHADOW);
}

void ui_draw_pressed(gfx_t *g, int x, int y, int w, int h)
{
    gfx_bevel(g, x, y, w, h, C_DKSHADOW, C_HILIGHT);
    gfx_bevel(g, x + 1, y + 1, w - 2, h - 2, C_SHADOW, C_FACE);
}

void ui_draw_sunken(gfx_t *g, int x, int y, int w, int h)
{
    gfx_bevel(g, x, y, w, h, C_SHADOW, C_HILIGHT);
    gfx_bevel(g, x + 1, y + 1, w - 2, h - 2, C_DKSHADOW, C_FACE);
}

void ui_draw_groove(gfx_t *g, int x, int y, int w, int h)
{
    gfx_frame(g, x + 1, y + 1, w - 1, h - 1, C_HILIGHT);
    gfx_frame(g, x, y, w - 1, h - 1, C_SHADOW);
}

// Mnemonic underlines are always shown: the OS is meant to be driven from the keyboard.
bool ui_show_mnemonics(void) { return true; }

void ui_draw_button(gfx_t *g, int x, int y, int w, int h, const char *label, const bitmap_t *icon,
                    bool pressed, bool focused, bool is_default, bool disabled)
{
    gfx_fill(g, x, y, w, h, C_FACE);
    if (is_default) { gfx_frame(g, x, y, w, h, C_DKSHADOW); x++; y++; w -= 2; h -= 2; }
    if (pressed) ui_draw_pressed(g, x, y, w, h); else ui_draw_raised(g, x, y, w, h);
    int o = pressed ? 1 : 0;
    int tw = label ? gfx_label_w(label, FONT_UI) : 0;
    int iw = icon ? icon->w + (tw ? 4 : 0) : 0;
    int cx = x + (w - tw - iw) / 2 + o;
    if (cx < x + 3) cx = x + 3;
    if (icon) gfx_bitmap(g, cx, y + (h - icon->h) / 2 + o, icon);
    if (label) {
        int ty = y + (h - FONT_UI_H) / 2 + o;
        if (disabled) {
            gfx_label(g, cx + iw + 1, ty + 1, label, FONT_UI, C_HILIGHT, true);
            gfx_label(g, cx + iw, ty, label, FONT_UI, C_TEXT_DIS, true);
        } else {
            gfx_label(g, cx + iw, ty, label, FONT_UI, C_TEXT, true);
        }
    }
    if (focused) gfx_dotted(g, x + 3, y + 3, w - 6, h - 6, C_BLACK);
}

// ---------------------------------------------------------------- widget core

static bool s_caret = true;
static void caret_tick(void *arg)
{
    s_caret = !s_caret;
    window_t *a = wm_active();
    if (a && a->focus && (a->focus->flags & WF_CARET)) ui_invalidate(a->focus);
}
bool ui_caret_on(void) { return s_caret; }
static void caret_reset(void) { s_caret = true; }

void *ui_new(window_t *win, const widget_class_t *cls, size_t size, int x, int y, int w, int h)
{
    static bool timer;
    if (!timer) { timer = true; timer_add(500, true, caret_tick, NULL); }
    widget_t *wd = heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL);
    if (!wd) wd = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
    if (!wd) return NULL;
    wd->cls = cls;
    wd->win = win;
    wd->r = rect_make(x, y, w, h);
    wd->prev = win->widgets_tail;
    if (win->widgets_tail) win->widgets_tail->next = wd; else win->widgets = wd;
    win->widgets_tail = wd;
    return wd;
}

void ui_delete(widget_t *w)
{
    window_t *win = w->win;
    if (w->cls->destroy) w->cls->destroy(w);
    if (w->prev) w->prev->next = w->next; else win->widgets = w->next;
    if (w->next) w->next->prev = w->prev; else win->widgets_tail = w->prev;
    if (win->focus == w) win->focus = NULL;
    if (win->def_btn == w) win->def_btn = NULL;
    if (win->cancel_btn == w) win->cancel_btn = NULL;
    free(w->text);
    free(w);
}

void ui_invalidate(widget_t *w)
{
    window_t *win = w->win;
    if (win->flags & (WIN_HIDDEN | WIN_MINIMIZED | WIN_DEAD)) return;
    rect_t c, r;
    wm_client_rect(win, &c);
    r = rect_make(c.x + w->r.x, c.y + w->r.y, w->r.w, w->r.h);
    if (rect_intersect(&r, &r, &c)) comp_invalidate_rect(&r);
}

void ui_set_text(widget_t *w, const char *text)
{
    if (w->text && text && !strcmp(w->text, text)) return;
    free(w->text);
    w->text = text ? strdup(text) : NULL;
    ui_invalidate(w);
}

void ui_set_rect(widget_t *w, int x, int y, int ww, int hh)
{
    if (w->r.x == x && w->r.y == y && w->r.w == ww && w->r.h == hh) return;
    ui_invalidate(w);
    w->r = rect_make(x, y, ww, hh);
    ui_invalidate(w);
}

void ui_focus(widget_t *w)
{
    window_t *win = w->win;
    widget_t *old = win->focus;
    caret_reset();
    if (old == w) return;
    win->focus = w;
    if (old) { if (old->cls->focus) old->cls->focus(old, false); ui_invalidate(old); }
    if (w->cls->focus) w->cls->focus(w, true);
    ui_invalidate(w);
}

void ui_enable(widget_t *w, bool on)
{
    bool was = !(w->flags & WF_DISABLED);
    if (was == on) return;
    if (on) w->flags &= ~WF_DISABLED; else w->flags |= WF_DISABLED;
    ui_invalidate(w);
}

void ui_show(widget_t *w, bool on)
{
    bool was = !(w->flags & WF_HIDDEN);
    if (was == on) return;
    if (!on) ui_invalidate(w);
    if (on) w->flags &= ~WF_HIDDEN; else w->flags |= WF_HIDDEN;
    if (on) ui_invalidate(w);
}

bool ui_focused(const widget_t *w)
{
    return w->win->focus == w && wm_active() == w->win && !shell_has_focus() && menubar_owner() != w->win;
}

void ui_notify(widget_t *w, int code)
{
    if (w->on_action) w->on_action(w, code);
}

// ---------------------------------------------------------------- label / group box / icon

static void label_paint(widget_t *w, gfx_t *g)
{
    if (!w->text) return;
    const char *s = w->text;
    int y = 0;
    uint8_t col = (w->flags & WF_DISABLED) ? C_TEXT_DIS : C_TEXT;
    while (*s) {
        const char *nl = strchr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)strlen(s);
        if (!nl && !strchr(w->text, '\n')) gfx_label(g, 0, y, s, FONT_UI, col, true);
        else gfx_text(g, 0, y, s, len, FONT_UI, col);
        y += FONT_UI_H + 2;
        if (!nl) break;
        s = nl + 1;
    }
}
static const widget_class_t label_class = {"label", label_paint, NULL, NULL, NULL, NULL};

widget_t *ui_label(window_t *win, int x, int y, int w, int h, const char *text)
{
    widget_t *wd = ui_new(win, &label_class, sizeof(widget_t), x, y, w, h);
    if (wd) wd->text = strdup(text);
    return wd;
}

static void group_paint(widget_t *w, gfx_t *g)
{
    ui_draw_groove(g, 0, 6, w->r.w, w->r.h - 6);
    if (w->text && w->text[0]) {
        int tw = gfx_text_w(w->text, -1, FONT_UI);
        gfx_fill(g, 8, 0, tw + 6, FONT_UI_H, C_FACE);
        gfx_text(g, 11, 0, w->text, -1, FONT_UI, C_TEXT);
    }
}
static const widget_class_t group_class = {"group", group_paint, NULL, NULL, NULL, NULL};

widget_t *ui_groupbox(window_t *win, int x, int y, int w, int h, const char *text)
{
    widget_t *wd = ui_new(win, &group_class, sizeof(widget_t), x, y, w, h);
    if (wd && text) wd->text = strdup(text);
    return wd;
}

static void icon_paint(widget_t *w, gfx_t *g) { if (w->user) gfx_bitmap(g, 0, 0, w->user); }
static const widget_class_t icon_class = {"icon", icon_paint, NULL, NULL, NULL, NULL};

widget_t *ui_icon(window_t *win, int x, int y, const bitmap_t *bmp)
{
    widget_t *wd = ui_new(win, &icon_class, sizeof(widget_t), x, y, bmp->w, bmp->h);
    if (wd) wd->user = (void *)bmp;
    return wd;
}

// ---------------------------------------------------------------- button

typedef struct { widget_t w; bool pressed, over; } button_t;

static void button_paint(widget_t *w, gfx_t *g)
{
    button_t *b = (button_t *)w;
    bool foc = ui_focused(w);
    // the default ring follows the focus when another button has it
    window_t *win = w->win;
    bool other_btn = win->focus && win->focus != w && !strcmp(win->focus->cls->name, "button");
    bool def = wm_active() == win && (foc || (win->def_btn == w && !other_btn));
    ui_draw_button(g, 0, 0, w->r.w, w->r.h, w->text, NULL, b->pressed && b->over, foc, def, w->flags & WF_DISABLED);
}

static bool button_key(widget_t *w, const event_t *e)
{
    if (e->key == K_SPACE && !(e->mods & (MOD_CTRL | MOD_ALT))) { ui_notify(w, UI_CLICK); return true; }
    return false;
}

static void button_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    button_t *b = (button_t *)w;
    bool over = x >= 0 && y >= 0 && x < w->r.w && y < w->r.h;
    if (type == EV_MOUSE_DOWN && btn == BTN_LEFT) { b->pressed = true; b->over = true; ui_invalidate(w); }
    else if (type == EV_MOUSE_MOVE && b->pressed && over != b->over) { b->over = over; ui_invalidate(w); }
    else if (type == EV_MOUSE_UP && btn == BTN_LEFT && b->pressed) {
        b->pressed = false;
        ui_invalidate(w);
        if (over) ui_notify(w, UI_CLICK);
    }
}

static void button_focus(widget_t *w, bool gained)
{
    // the default button's ring moves, repaint it
    if (w->win->def_btn && w->win->def_btn != w) ui_invalidate(w->win->def_btn);
}

static const widget_class_t button_class = {"button", button_paint, button_key, button_mouse, button_focus, NULL};

widget_t *ui_button(window_t *win, int x, int y, int w, int h, const char *text, void (*on_click)(widget_t *, int))
{
    widget_t *wd = ui_new(win, &button_class, sizeof(button_t), x, y, w, h);
    if (!wd) return NULL;
    wd->text = strdup(text);
    wd->flags = WF_TABSTOP;
    wd->on_action = on_click;
    return wd;
}

// ---------------------------------------------------------------- check box / radio

typedef struct { widget_t w; bool checked; int group; } check_t;
static const widget_class_t check_class, radio_class;

static const uint16_t g_tick[7] = {0x0200, 0x0600, 0x8E00, 0xDC00, 0xF800, 0x7000, 0x2000};

static void check_paint(widget_t *w, gfx_t *g)
{
    check_t *c = (check_t *)w;
    bool radio = w->cls == &radio_class;
    bool dis = w->flags & WF_DISABLED;
    int by = (w->r.h - 13) / 2;
    if (radio) {
        // a 12x12 diamond-ish circle
        static const uint16_t ring_dk[12] = {0x0F00, 0x30C0, 0x4000, 0x4000, 0x8000, 0x8000, 0x8000, 0x8000, 0x4000, 0x4000, 0, 0};
        static const uint16_t ring_lt[12] = {0, 0, 0x0020, 0x0020, 0x0010, 0x0010, 0x0010, 0x0010, 0x0020, 0x0020, 0x30C0, 0x0F00};
        static const uint16_t fill[12] = {0, 0x0F00, 0x3FC0, 0x3FC0, 0x7FE0, 0x7FE0, 0x7FE0, 0x7FE0, 0x3FC0, 0x3FC0, 0x0F00, 0};
        static const uint16_t dot[12] = {0, 0, 0, 0, 0x0F00, 0x1F80, 0x1F80, 0x0F00, 0, 0, 0, 0};
        gfx_glyph16(g, 0, by, fill, 12, 12, dis ? C_FACE : C_FIELD);
        gfx_glyph16(g, 0, by, ring_dk, 12, 12, C_SHADOW);
        gfx_glyph16(g, 0, by, ring_lt, 12, 12, C_HILIGHT);
        if (c->checked) gfx_glyph16(g, 0, by, dot, 12, 12, dis ? C_TEXT_DIS : C_TEXT);
    } else {
        gfx_fill(g, 0, by, 13, 13, dis ? C_FACE : C_FIELD);
        ui_draw_sunken(g, 0, by, 13, 13);
        if (c->checked) gfx_glyph16(g, 3, by + 3, g_tick, 7, 7, dis ? C_TEXT_DIS : C_TEXT);
    }
    int ty = (w->r.h - FONT_UI_H) / 2;
    gfx_label(g, 18, ty, w->text, FONT_UI, dis ? C_TEXT_DIS : C_TEXT, true);
    if (ui_focused(w)) gfx_dotted(g, 16, ty - 1, gfx_label_w(w->text, FONT_UI) + 4, FONT_UI_H + 3, C_BLACK);
}

static void radio_select(widget_t *w)
{
    check_t *c = (check_t *)w;
    for (widget_t *o = w->win->widgets; o; o = o->next) {
        if (o->cls == &radio_class && ((check_t *)o)->group == c->group && ((check_t *)o)->checked && o != w) {
            ((check_t *)o)->checked = false;
            ui_invalidate(o);
        }
    }
    if (!c->checked) { c->checked = true; ui_invalidate(w); ui_notify(w, UI_CHANGE); }
}

static widget_t *radio_neighbour(widget_t *w, bool back)
{
    check_t *c = (check_t *)w;
    for (widget_t *o = back ? w->prev : w->next; o; o = back ? o->prev : o->next) {
        if (o->cls == &radio_class && ((check_t *)o)->group == c->group && !(o->flags & (WF_DISABLED | WF_HIDDEN))) return o;
    }
    return NULL;
}

static bool check_key(widget_t *w, const event_t *e)
{
    check_t *c = (check_t *)w;
    if (e->mods & (MOD_CTRL | MOD_ALT)) return false;
    if (w->cls == &radio_class) {
        if (e->key == K_SPACE) { radio_select(w); return true; }
        widget_t *n = NULL;
        if (e->key == K_DOWN || e->key == K_RIGHT) n = radio_neighbour(w, false);
        else if (e->key == K_UP || e->key == K_LEFT) n = radio_neighbour(w, true);
        else return false;
        if (n) { ui_focus(n); radio_select(n); }
        return true;
    }
    if (e->key == K_SPACE) { c->checked = !c->checked; ui_invalidate(w); ui_notify(w, UI_CHANGE); return true; }
    return false;
}

static void check_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    if (type == EV_MOUSE_UP && btn == BTN_LEFT && x >= 0 && y >= 0 && x < w->r.w && y < w->r.h) {
        event_t e = {.type = EV_KEY, .key = K_SPACE, .down = 1};
        check_key(w, &e);
    }
}

static const widget_class_t check_class = {"check", check_paint, check_key, check_mouse, NULL, NULL};
static const widget_class_t radio_class = {"radio", check_paint, check_key, check_mouse, NULL, NULL};

widget_t *ui_checkbox(window_t *win, int x, int y, int w, const char *text, bool checked)
{
    check_t *c = ui_new(win, &check_class, sizeof(check_t), x, y, w, 18);
    if (!c) return NULL;
    c->w.text = strdup(text);
    c->w.flags = WF_TABSTOP | WF_WANT_ARROWS * 0;
    c->checked = checked;
    return &c->w;
}

widget_t *ui_radio(window_t *win, int x, int y, int w, const char *text, int group, bool checked)
{
    check_t *c = ui_new(win, &radio_class, sizeof(check_t), x, y, w, 18);
    if (!c) return NULL;
    c->w.text = strdup(text);
    c->w.flags = WF_TABSTOP | WF_WANT_ARROWS;
    c->checked = checked;
    c->group = group;
    return &c->w;
}

bool ui_checked(const widget_t *w) { return ((const check_t *)w)->checked; }

void ui_set_checked(widget_t *w, bool on)
{
    check_t *c = (check_t *)w;
    if (c->checked == on) return;
    if (w->cls == &radio_class && on) {
        for (widget_t *o = w->win->widgets; o; o = o->next) {
            if (o->cls == &radio_class && ((check_t *)o)->group == c->group && o != w && ((check_t *)o)->checked) {
                ((check_t *)o)->checked = false;
                ui_invalidate(o);
            }
        }
    }
    c->checked = on;
    ui_invalidate(w);
}

int ui_radio_selected(window_t *win, int group)
{
    int i = 0;
    for (widget_t *o = win->widgets; o; o = o->next) {
        if (o->cls != &radio_class || ((check_t *)o)->group != group) continue;
        if (((check_t *)o)->checked) return i;
        i++;
    }
    return -1;
}

// ---------------------------------------------------------------- text field

typedef struct { widget_t w; char *buf; int cap, len, cur, anchor, scroll; bool dragging, mask; } field_t;

// what is drawn: the text itself, or bullets for a password
static const char *field_shown(field_t *f)
{
    static char stars[130];
    if (!f->mask) return f->buf;
    int n = f->len < 128 ? f->len : 128;
    memset(stars, '*', n);
    stars[n] = 0;
    return stars;
}

static int field_x_of(field_t *f, int idx) { return gfx_text_w(field_shown(f), idx, FONT_UI); }

static int field_index_at(field_t *f, int px)
{
    int x = 0;
    for (int i = 0; i < f->len; i++) {
        int adv = gfx_text_w(f->mask ? "*" : f->buf + i, 1, FONT_UI);
        if (px < x + adv / 2) return i;
        x += adv;
    }
    return f->len;
}

static void field_scroll_to_cursor(field_t *f)
{
    int inner = f->w.r.w - 8;
    int cx = field_x_of(f, f->cur);
    if (cx - f->scroll > inner - 2) f->scroll = cx - inner + 2;
    if (cx - f->scroll < 0) f->scroll = cx;
    if (f->scroll < 0) f->scroll = 0;
}

static void field_paint(widget_t *w, gfx_t *g)
{
    field_t *f = (field_t *)w;
    bool dis = w->flags & WF_DISABLED;
    gfx_fill(g, 0, 0, w->r.w, w->r.h, dis ? C_FACE : C_FIELD);
    ui_draw_sunken(g, 0, 0, w->r.w, w->r.h);
    gfx_state_t st;
    gfx_save(g, &st);
    if (!gfx_clip(g, 3, 2, w->r.w - 6, w->r.h - 4)) { gfx_restore(g, &st); return; }
    int tx = 4 - f->scroll, ty = (w->r.h - FONT_UI_H) / 2;
    const char *txt = field_shown(f);
    bool foc = ui_focused(w);
    int s0 = f->cur < f->anchor ? f->cur : f->anchor, s1 = f->cur < f->anchor ? f->anchor : f->cur;
    if (foc && s1 > s0) {
        int xa = field_x_of(f, s0), xb = field_x_of(f, s1);
        gfx_text(g, tx, ty, txt, s0, FONT_UI, C_TEXT);
        gfx_fill(g, tx + xa, ty - 1, xb - xa, FONT_UI_H + 2, C_SEL);
        gfx_text(g, tx + xa, ty, txt + s0, s1 - s0, FONT_UI, C_SEL_TEXT);
        gfx_text(g, tx + xb, ty, txt + s1, f->len - s1, FONT_UI, C_TEXT);
    } else {
        gfx_text(g, tx, ty, txt, f->len, FONT_UI, dis ? C_TEXT_DIS : C_TEXT);
    }
    if (foc && ui_caret_on()) gfx_vline(g, tx + field_x_of(f, f->cur) - 1, ty - 1, FONT_UI_H + 2, C_BLACK);
    gfx_restore(g, &st);
}

static void field_delete_sel(field_t *f)
{
    int s0 = f->cur < f->anchor ? f->cur : f->anchor, s1 = f->cur < f->anchor ? f->anchor : f->cur;
    if (s1 == s0) return;
    memmove(f->buf + s0, f->buf + s1, f->len - s1 + 1);
    f->len -= s1 - s0;
    f->cur = f->anchor = s0;
}

static void field_insert(field_t *f, const char *s, int n)
{
    field_delete_sel(f);
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\n' || c == '\r') break;
        if ((unsigned char)c < 32 || f->len >= f->cap) continue;
        memmove(f->buf + f->cur + 1, f->buf + f->cur, f->len - f->cur + 1);
        f->buf[f->cur++] = c;
        f->len++;
    }
    f->anchor = f->cur;
}

static bool field_key(widget_t *w, const event_t *e)
{
    field_t *f = (field_t *)w;
    bool shift = e->mods & MOD_SHIFT, ctrl = e->mods & MOD_CTRL;
    bool changed = false, moved = true;
    int s0 = f->cur < f->anchor ? f->cur : f->anchor, s1 = f->cur < f->anchor ? f->anchor : f->cur;
    if (e->mods & MOD_ALT) return false;
    if (ctrl) {
        switch (e->key) {
        case K_A: f->anchor = 0; f->cur = f->len; break;
        case K_C: if (s1 > s0 && !f->mask) clip_set(f->buf + s0, s1 - s0); break;
        case K_X: if (s1 > s0 && !f->mask) { clip_set(f->buf + s0, s1 - s0); field_delete_sel(f); changed = true; } break;
        case K_V: { int n; const char *c = clip_get(&n); field_insert(f, c, n); changed = true; break; }
        case K_LEFT: f->cur = 0; if (!shift) f->anchor = f->cur; break;
        case K_RIGHT: f->cur = f->len; if (!shift) f->anchor = f->cur; break;
        default: return false;
        }
    } else {
        switch (e->key) {
        case K_LEFT:
            if (!shift && s1 > s0) f->cur = s0;
            else if (f->cur > 0) f->cur -= gfx_glyph_back(f->buf, f->cur);  // whole glyph
            if (!shift) f->anchor = f->cur;
            break;
        case K_RIGHT:
            if (!shift && s1 > s0) f->cur = s1;
            else if (f->cur < f->len) {
                int n;
                gfx_glyph(f->buf, f->cur, f->len, &n);
                f->cur += n;
            }
            if (!shift) f->anchor = f->cur;
            break;
        case K_HOME: f->cur = 0; if (!shift) f->anchor = 0; break;
        case K_END: f->cur = f->len; if (!shift) f->anchor = f->cur; break;
        case K_BKSP:
            if (s1 > s0) field_delete_sel(f);
            else if (f->cur > 0) { f->anchor = f->cur - gfx_glyph_back(f->buf, f->cur); field_delete_sel(f); }
            changed = true;
            break;
        case K_DEL:
            if (s1 > s0) field_delete_sel(f);
            else if (f->cur < f->len) {
                int n;
                gfx_glyph(f->buf, f->cur, f->len, &n);
                f->anchor = f->cur + n;
                field_delete_sel(f);
            }
            changed = true;
            break;
        default:
            if (e->chs[0]) { field_insert(f, e->chs, (int)strlen(e->chs)); changed = true; }
            else moved = false;
        }
    }
    if (!moved) return false;
    field_scroll_to_cursor(f);
    ui_invalidate(w);
    if (changed) ui_notify(w, UI_CHANGE);
    return true;
}

static void field_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    field_t *f = (field_t *)w;
    if (type == EV_MOUSE_DOWN && btn == BTN_LEFT) {
        if (clicks == 2) { f->anchor = 0; f->cur = f->len; }
        else { f->cur = field_index_at(f, x - 4 + f->scroll); if (!(input_mods() & MOD_SHIFT)) f->anchor = f->cur; f->dragging = true; }
        ui_invalidate(w);
    } else if (type == EV_MOUSE_MOVE && f->dragging) {
        f->cur = field_index_at(f, x - 4 + f->scroll);
        field_scroll_to_cursor(f);
        ui_invalidate(w);
    } else if (type == EV_MOUSE_UP) {
        f->dragging = false;
    }
}

static void field_focus(widget_t *w, bool gained)
{
    field_t *f = (field_t *)w;
    if (gained && !f->dragging) { f->anchor = 0; f->cur = f->len; field_scroll_to_cursor(f); }
}

static void field_destroy(widget_t *w) { free(((field_t *)w)->buf); }

static const widget_class_t field_class = {"field", field_paint, field_key, field_mouse, field_focus, field_destroy};

widget_t *ui_textfield(window_t *win, int x, int y, int w, const char *text, int max_len)
{
    field_t *f = ui_new(win, &field_class, sizeof(field_t), x, y, w, 20);
    if (!f) return NULL;
    f->cap = max_len;
    f->buf = calloc(1, max_len + 1);
    if (text) { strlcpy(f->buf, text, max_len + 1); f->len = strlen(f->buf); }
    f->cur = f->anchor = f->len;
    f->w.flags = WF_TABSTOP | WF_IBEAM | WF_WANT_CHARS | WF_CARET | WF_WANT_ARROWS;
    return &f->w;
}

const char *ui_textfield_get(widget_t *w) { return ((field_t *)w)->buf; }
void ui_textfield_mask(widget_t *w, bool on) { ((field_t *)w)->mask = on; ui_invalidate(w); }

void ui_textfield_set(widget_t *w, const char *text)
{
    field_t *f = (field_t *)w;
    strlcpy(f->buf, text, f->cap + 1);
    f->len = strlen(f->buf);
    f->cur = f->anchor = f->len;
    f->scroll = 0;
    ui_invalidate(w);
}

void ui_textfield_select_all(widget_t *w)
{
    field_t *f = (field_t *)w;
    f->anchor = 0; f->cur = f->len;
    ui_invalidate(w);
}

// ---------------------------------------------------------------- slider

typedef struct { widget_t w; int min, max, val; bool dragging; } slider_t;

static int slider_thumb_x(slider_t *s)
{
    int range = s->max - s->min;
    return range > 0 ? (s->val - s->min) * (s->w.r.w - 11) / range : 0;
}

static void slider_paint(widget_t *w, gfx_t *g)
{
    slider_t *s = (slider_t *)w;
    int my = w->r.h / 2;
    ui_draw_sunken(g, 2, my - 2, w->r.w - 4, 4);
    int range = s->max - s->min;
    for (int i = 0; i <= range && range <= 20; i++) gfx_vline(g, 5 + i * (w->r.w - 11) / range, w->r.h - 4, 3, C_SHADOW);
    int tx = slider_thumb_x(s);
    gfx_fill(g, tx, 2, 11, w->r.h - 8, C_FACE);
    ui_draw_raised(g, tx, 2, 11, w->r.h - 8);
    if (ui_focused(w)) gfx_dotted(g, 0, 0, w->r.w, w->r.h, C_BLACK);
}

static void slider_setv(slider_t *s, int v)
{
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    if (v == s->val) return;
    s->val = v;
    ui_invalidate(&s->w);
    ui_notify(&s->w, UI_CHANGE);
}

static bool slider_key(widget_t *w, const event_t *e)
{
    slider_t *s = (slider_t *)w;
    switch (e->key) {
    case K_LEFT: case K_DOWN: slider_setv(s, s->val - 1); return true;
    case K_RIGHT: case K_UP: slider_setv(s, s->val + 1); return true;
    case K_HOME: slider_setv(s, s->min); return true;
    case K_END: slider_setv(s, s->max); return true;
    }
    return false;
}

static void slider_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    slider_t *s = (slider_t *)w;
    if (type == EV_MOUSE_DOWN && btn == BTN_LEFT) s->dragging = true;
    if (type == EV_MOUSE_UP) s->dragging = false;
    if (s->dragging && (type == EV_MOUSE_DOWN || type == EV_MOUSE_MOVE)) {
        int range = s->max - s->min, span = w->r.w - 11;
        slider_setv(s, s->min + ((x - 5) * range + span / 2) / (span > 0 ? span : 1));
    }
    if (type == EV_WHEEL) slider_setv(s, s->val + (btn > 0 ? 1 : -1));
}

static const widget_class_t slider_class = {"slider", slider_paint, slider_key, slider_mouse, NULL, NULL};

widget_t *ui_slider(window_t *win, int x, int y, int w, int min, int max, int value)
{
    slider_t *s = ui_new(win, &slider_class, sizeof(slider_t), x, y, w, 24);
    if (!s) return NULL;
    s->min = min; s->max = max; s->val = value;
    s->w.flags = WF_TABSTOP | WF_WANT_ARROWS;
    return &s->w;
}

int ui_slider_get(const widget_t *w) { return ((const slider_t *)w)->val; }
void ui_slider_set(widget_t *w, int v) { slider_t *s = (slider_t *)w; if (v < s->min) v = s->min; if (v > s->max) v = s->max; s->val = v; ui_invalidate(w); }

// ---------------------------------------------------------------- scrollbar helper

static const uint16_t g_arr_up[4] = {0x1000, 0x3800, 0x7C00, 0xFE00};
static const uint16_t g_arr_dn[4] = {0xFE00, 0x7C00, 0x3800, 0x1000};
static const uint16_t g_arr_lf[7] = {0x1000, 0x3000, 0x7000, 0xF000, 0x7000, 0x3000, 0x1000};
static const uint16_t g_arr_rt[7] = {0x8000, 0xC000, 0xE000, 0xF000, 0xE000, 0xC000, 0x8000};

void scroll_clamp(scroll_t *s)
{
    int max = s->total - s->page;
    if (s->pos > max) s->pos = max;
    if (s->pos < 0) s->pos = 0;
}

// track geometry along the scroll axis
static void sb_geom(const scroll_t *s, int len, int *thumb_pos, int *thumb_len)
{
    int track = len - 2 * SCROLL_W;
    if (track < 8 || s->total <= s->page || s->total <= 0) { *thumb_pos = 0; *thumb_len = track > 0 ? track : 0; return; }
    int tl = (int)((int64_t)track * s->page / s->total);
    if (tl < 10) tl = 10;
    if (tl > track) tl = track;
    int max = s->total - s->page;
    *thumb_len = tl;
    *thumb_pos = max > 0 ? (int)((int64_t)(track - tl) * s->pos / max) : 0;
}

void scroll_paint(gfx_t *g, const rect_t *r, const scroll_t *s, bool horizontal)
{
    int len = horizontal ? r->w : r->h;
    int tp, tl;
    sb_geom(s, len, &tp, &tl);
    bool active = s->total > s->page;
    gfx_checker(g, r->x, r->y, r->w, r->h, C_HILIGHT, C_FACE);
    // arrow buttons
    for (int i = 0; i < 2; i++) {
        int bx = horizontal ? (i ? r->x + r->w - SCROLL_W : r->x) : r->x;
        int by = horizontal ? r->y : (i ? r->y + r->h - SCROLL_W : r->y);
        bool down = s->pressed == i + 1;
        gfx_fill(g, bx, by, SCROLL_W, SCROLL_W, C_FACE);
        if (down) ui_draw_pressed(g, bx, by, SCROLL_W, SCROLL_W); else ui_draw_raised(g, bx, by, SCROLL_W, SCROLL_W);
        uint8_t col = active ? C_BLACK : C_SHADOW;
        int o = down ? 1 : 0;
        if (horizontal) gfx_glyph16(g, bx + 6 + o, by + 4 + o, i ? g_arr_rt : g_arr_lf, 4, 7, col);
        else gfx_glyph16(g, bx + 4 + o, by + 6 + o, i ? g_arr_dn : g_arr_up, 7, 4, col);
    }
    if (active && tl > 0) {
        int tx = horizontal ? r->x + SCROLL_W + tp : r->x;
        int ty = horizontal ? r->y : r->y + SCROLL_W + tp;
        int tw = horizontal ? tl : SCROLL_W, th = horizontal ? SCROLL_W : tl;
        gfx_fill(g, tx, ty, tw, th, C_FACE);
        ui_draw_raised(g, tx, ty, tw, th);
    }
}

bool scroll_mouse(scroll_t *s, const rect_t *r, bool horizontal, int type, int x, int y, int line)
{
    int len = horizontal ? r->w : r->h;
    int p = horizontal ? x - r->x : y - r->y;
    int tp, tl;
    sb_geom(s, len, &tp, &tl);
    int old = s->pos;
    if (type == EV_MOUSE_DOWN) {
        if (s->total <= s->page) return false;
        if (p < SCROLL_W) { s->pressed = 1; s->pos -= line; }
        else if (p >= len - SCROLL_W) { s->pressed = 2; s->pos += line; }
        else if (p < SCROLL_W + tp) s->pos -= s->page;
        else if (p >= SCROLL_W + tp + tl) s->pos += s->page;
        else { s->pressed = 3; s->drag_off = p - SCROLL_W - tp; }
    } else if (type == EV_MOUSE_MOVE && s->pressed == 3) {
        int track = len - 2 * SCROLL_W - tl;
        int max = s->total - s->page;
        int np = p - SCROLL_W - s->drag_off;
        s->pos = track > 0 ? (int)((int64_t)np * max / track) : 0;
    } else if (type == EV_MOUSE_UP) {
        bool was = s->pressed;
        s->pressed = 0;
        return was;
    }
    scroll_clamp(s);
    return s->pos != old || type == EV_MOUSE_DOWN;
}

// ---------------------------------------------------------------- list view

#define LV_ROW_H 18
#define LV_HDR_H 18
typedef struct {
    widget_t w;
    lv_get_fn get;
    int count, sel;
    scroll_t sb;
    int ncols;
    const char *titles[4];
    int widths[4];
    bool sb_drag;
    char typed[16];
    uint32_t typed_ms;
    int ctx_x, ctx_y;
} listview_t;

static int lv_rows_visible(listview_t *lv)
{
    int h = lv->w.r.h - 4 - (lv->ncols ? LV_HDR_H : 0);
    return h / LV_ROW_H > 0 ? h / LV_ROW_H : 1;
}

static void lv_sync(listview_t *lv)
{
    lv->sb.total = lv->count;
    lv->sb.page = lv_rows_visible(lv);
    scroll_clamp(&lv->sb);
}

static void lv_ensure_visible(listview_t *lv)
{
    lv_sync(lv);
    if (lv->sel < 0) return;
    if (lv->sel < lv->sb.pos) lv->sb.pos = lv->sel;
    if (lv->sel >= lv->sb.pos + lv->sb.page) lv->sb.pos = lv->sel - lv->sb.page + 1;
    scroll_clamp(&lv->sb);
}

static void lv_paint(widget_t *w, gfx_t *g)
{
    listview_t *lv = (listview_t *)w;
    lv_sync(lv);
    gfx_fill(g, 0, 0, w->r.w, w->r.h, C_FIELD);
    ui_draw_sunken(g, 0, 0, w->r.w, w->r.h);
    bool need_sb = lv->count > lv->sb.page;
    int inner_w = w->r.w - 4 - (need_sb ? SCROLL_W : 0);
    int y0 = 2;
    if (lv->ncols) {
        int x = 2;
        for (int c = 0; c < lv->ncols; c++) {
            int cw = c == lv->ncols - 1 ? inner_w + 2 - x + (need_sb ? SCROLL_W : 0) : lv->widths[c];
            gfx_fill(g, x, 2, cw, LV_HDR_H, C_FACE);
            ui_draw_raised(g, x, 2, cw, LV_HDR_H);
            gfx_text(g, x + 6, 5, lv->titles[c], -1, FONT_UI, C_TEXT);
            x += cw;
        }
        y0 += LV_HDR_H;
    }
    bool foc = ui_focused(w);
    gfx_state_t st;
    gfx_save(g, &st);
    if (gfx_clip(g, 2, y0, inner_w, w->r.h - 2 - y0)) {
        for (int i = 0; i < lv->sb.page + 1; i++) {
            int idx = lv->sb.pos + i;
            if (idx >= lv->count) break;
            int y = y0 + i * LV_ROW_H;
            if (!gfx_visible(g, 2, y, inner_w, LV_ROW_H)) continue;
            lv_item_t it = {0};
            lv->get(w, idx, &it);
            bool sel = idx == lv->sel;
            uint8_t fg = C_TEXT;
            int tx = 4;
            if (it.icon) tx += 20;
            if (sel) {
                int tw = lv->ncols > 1 ? lv->widths[0] - tx + 2 : inner_w - tx + 2;
                gfx_fill(g, tx - 2 + 2, y, tw - 2, LV_ROW_H, foc ? C_SEL : C_FACE);
                if (foc) { fg = C_SEL_TEXT; gfx_dotted(g, tx, y, tw - 2, LV_ROW_H, C_YELLOW); }
            }
            if (it.icon) gfx_bitmap(g, 4, y + 1, it.icon);
            if (lv->ncols > 1) {
                gfx_state_t cs;
                gfx_save(g, &cs);
                if (gfx_clip(g, 2, y, lv->widths[0] - 2, LV_ROW_H) && it.text) gfx_text(g, tx + 3, y + 3, it.text, -1, FONT_UI, fg);
                gfx_restore(g, &cs);
                int x = 2 + lv->widths[0];
                for (int c = 1; c < lv->ncols; c++) {
                    int cw = c == lv->ncols - 1 ? inner_w + 2 - x : lv->widths[c];
                    gfx_save(g, &cs);
                    if (it.col[c - 1] && gfx_clip(g, x, y, cw - 2, LV_ROW_H)) gfx_text(g, x + 6, y + 3, it.col[c - 1], -1, FONT_UI, C_TEXT);
                    gfx_restore(g, &cs);
                    x += cw;
                }
            } else if (it.text) {
                gfx_text(g, tx + 3, y + 3, it.text, -1, FONT_UI, fg);
            }
        }
    }
    gfx_restore(g, &st);
    if (need_sb) {
        rect_t sr = {w->r.w - 2 - SCROLL_W, y0, SCROLL_W, w->r.h - 2 - y0};
        scroll_paint(g, &sr, &lv->sb, false);
    }
    if (foc && lv->count == 0) gfx_dotted(g, 4, y0 + 2, inner_w - 4, LV_ROW_H - 2, C_BLACK);
}

static void lv_set_sel(listview_t *lv, int idx, bool notify)
{
    if (idx >= lv->count) idx = lv->count - 1;
    if (idx < 0) idx = lv->count ? 0 : -1;
    bool changed = idx != lv->sel;
    lv->sel = idx;
    lv_ensure_visible(lv);
    ui_invalidate(&lv->w);
    if (changed && notify) ui_notify(&lv->w, UI_SELECT);
}

static bool lv_key(widget_t *w, const event_t *e)
{
    listview_t *lv = (listview_t *)w;
    if (e->mods & (MOD_CTRL | MOD_ALT)) return false;
    lv_sync(lv);
    switch (e->key) {
    case K_UP: lv_set_sel(lv, lv->sel - 1, true); return true;
    case K_DOWN: lv_set_sel(lv, lv->sel + 1, true); return true;
    case K_HOME: lv_set_sel(lv, 0, true); return true;
    case K_END: lv_set_sel(lv, lv->count - 1, true); return true;
    case K_PGUP: lv_set_sel(lv, lv->sel - lv->sb.page, true); return true;
    case K_PGDN: lv_set_sel(lv, lv->sel + lv->sb.page, true); return true;
    case K_ENTER: case K_KP_ENTER:
        if (lv->sel >= 0) ui_notify(w, UI_ACTIVATE);
        return true;
    case K_MENU:
        if (lv->sel >= 0) {
            rect_t c;
            wm_client_rect(w->win, &c);
            lv->ctx_x = c.x + w->r.x + 40;
            lv->ctx_y = c.y + w->r.y + 2 + (lv->ncols ? LV_HDR_H : 0) + (lv->sel - lv->sb.pos + 1) * LV_ROW_H;
            ui_notify(w, UI_CONTEXT);
        }
        return true;
    }
    if (e->ch > ' ' && e->ch < 127) {
        // type to find: jump to the next item starting with what was typed
        uint32_t now = os_uptime_ms();
        int n = strlen(lv->typed);
        if (now - lv->typed_ms > 900) n = 0;
        lv->typed_ms = now;
        if (n < (int)sizeof(lv->typed) - 1) { lv->typed[n++] = tolower((unsigned char)e->ch); lv->typed[n] = 0; }
        int start = lv->sel < 0 ? 0 : lv->sel + (n == 1 ? 1 : 0);
        for (int k = 0; k < lv->count; k++) {
            int idx = (start + k) % lv->count;
            lv_item_t it = {0};
            lv->get(w, idx, &it);
            if (it.text && !strncasecmp(it.text, lv->typed, n)) { lv_set_sel(lv, idx, true); break; }
        }
        return true;
    }
    return false;
}

static void lv_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    listview_t *lv = (listview_t *)w;
    lv_sync(lv);
    int y0 = 2 + (lv->ncols ? LV_HDR_H : 0);
    bool need_sb = lv->count > lv->sb.page;
    rect_t sr = {w->r.w - 2 - SCROLL_W, y0, SCROLL_W, w->r.h - 2 - y0};
    if (type == EV_WHEEL) {
        lv->sb.pos -= btn * 3;
        scroll_clamp(&lv->sb);
        ui_invalidate(w);
        return;
    }
    if (lv->sb_drag || (need_sb && type == EV_MOUSE_DOWN && btn == BTN_LEFT && rect_has(&sr, x, y))) {
        if (type == EV_MOUSE_DOWN) lv->sb_drag = true;
        if (scroll_mouse(&lv->sb, &sr, false, type, x, y, 1)) ui_invalidate(w);
        if (type == EV_MOUSE_UP) lv->sb_drag = false;
        return;
    }
    if (type == EV_MOUSE_DOWN && y >= y0) {
        int idx = lv->sb.pos + (y - y0) / LV_ROW_H;
        if (idx < lv->count) {
            lv_set_sel(lv, idx, true);
            if (btn == BTN_LEFT && clicks == 2) ui_notify(w, UI_ACTIVATE);
        }
        if (btn == BTN_RIGHT) {
            rect_t c;
            wm_client_rect(w->win, &c);
            lv->ctx_x = c.x + w->r.x + x;
            lv->ctx_y = c.y + w->r.y + y;
            if (idx >= lv->count) lv_set_sel(lv, -1, true), lv->sel = -1, ui_invalidate(w);
            ui_notify(w, UI_CONTEXT);
        }
    }
}

static const widget_class_t lv_class = {"list", lv_paint, lv_key, lv_mouse, NULL, NULL};

widget_t *ui_listview(window_t *win, int x, int y, int w, int h, lv_get_fn get)
{
    listview_t *lv = ui_new(win, &lv_class, sizeof(listview_t), x, y, w, h);
    if (!lv) return NULL;
    lv->get = get;
    lv->sel = -1;
    lv->w.flags = WF_TABSTOP | WF_WANT_ENTER | WF_WANT_CHARS | WF_WANT_ARROWS;
    return &lv->w;
}

void ui_listview_columns(widget_t *w, int n, const char *const *titles, const int *widths)
{
    listview_t *lv = (listview_t *)w;
    lv->ncols = n > 4 ? 4 : n;
    for (int i = 0; i < lv->ncols; i++) { lv->titles[i] = titles[i]; lv->widths[i] = widths[i]; }
}

void ui_listview_set_count(widget_t *w, int count)
{
    listview_t *lv = (listview_t *)w;
    lv->count = count;
    if (lv->sel >= count) lv->sel = count - 1;
    if (lv->sel < 0 && count > 0) lv->sel = 0;
    lv_ensure_visible(lv);
    ui_invalidate(w);
}

int ui_listview_count(const widget_t *w) { return ((const listview_t *)w)->count; }
int ui_listview_sel(const widget_t *w) { return ((const listview_t *)w)->sel; }
void ui_listview_select(widget_t *w, int index) { lv_set_sel((listview_t *)w, index, false); }
void ui_listview_context_pos(widget_t *w, int *sx, int *sy)
{
    *sx = ((listview_t *)w)->ctx_x; *sy = ((listview_t *)w)->ctx_y;
}

// ---------------------------------------------------------------- drop-down list

// A closed field showing the chosen item, with an arrow button. The list opens as an overlay above
// every window, like a menu, so it needs no room inside the dialog. Only one is open at a time.
#define DD_MAX_ROWS 12
#define DD_BTN_W 16
typedef struct {
    widget_t w;
    lv_get_fn get;
    int count, sel;
    char typed[16];
    uint32_t typed_ms;
} dropdown_t;

static struct {
    dropdown_t *dd;                 // owner of the open list, NULL when closed
    rect_t r;                       // screen rectangle of the list
    int hot;                        // highlighted row
    scroll_t sb;
    bool sb_drag;
} s_pop;

static const uint16_t g_dd_arrow[4] = {0xFE00, 0x7C00, 0x3800, 0x1000};

static const char *dd_text(dropdown_t *d, int i, const bitmap_t **icon)
{
    lv_item_t it = {0};
    if (i >= 0 && i < d->count) d->get(&d->w, i, &it);
    if (icon) *icon = it.icon;
    return it.text ? it.text : "";
}

// type to find, for the closed field and the open list alike: the next item starting with the letters
static int dd_find(dropdown_t *d, char ch, int from)
{
    uint32_t now = os_uptime_ms();
    int n = strlen(d->typed);
    if (now - d->typed_ms > 900) n = 0;
    d->typed_ms = now;
    if (n < (int)sizeof(d->typed) - 1) { d->typed[n++] = tolower((unsigned char)ch); d->typed[n] = 0; }
    int start = from < 0 ? 0 : from + (n == 1 ? 1 : 0);
    for (int k = 0; k < d->count; k++) {
        int idx = (start + k) % d->count;
        if (!strncasecmp(dd_text(d, idx, NULL), d->typed, n)) return idx;
    }
    return -1;
}

static void dd_paint(widget_t *w, gfx_t *g)
{
    dropdown_t *d = (dropdown_t *)w;
    bool dis = w->flags & WF_DISABLED, open = s_pop.dd == d;
    gfx_fill(g, 0, 0, w->r.w, w->r.h, dis ? C_FACE : C_FIELD);
    ui_draw_sunken(g, 0, 0, w->r.w, w->r.h);
    int bx = w->r.w - 2 - DD_BTN_W, bh = w->r.h - 4;
    gfx_fill(g, bx, 2, DD_BTN_W, bh, C_FACE);
    if (open) ui_draw_pressed(g, bx, 2, DD_BTN_W, bh); else ui_draw_raised(g, bx, 2, DD_BTN_W, bh);
    int o = open ? 1 : 0;
    gfx_glyph16(g, bx + (DD_BTN_W - 7) / 2 + o, 2 + (bh - 4) / 2 + o, g_dd_arrow, 7, 4, dis ? C_TEXT_DIS : C_TEXT);
    gfx_state_t st;
    gfx_save(g, &st);
    if (gfx_clip(g, 2, 2, bx - 2, w->r.h - 4)) {
        const bitmap_t *icon;
        const char *txt = dd_text(d, d->sel, &icon);
        uint8_t fg = dis ? C_TEXT_DIS : C_TEXT;
        if (ui_focused(w) && !open) {
            gfx_fill(g, 3, 3, bx - 4, w->r.h - 6, C_SEL);
            gfx_dotted(g, 3, 3, bx - 4, w->r.h - 6, C_YELLOW);
            fg = C_SEL_TEXT;
        }
        int tx = 5;
        if (icon) { gfx_bitmap(g, tx, (w->r.h - 16) / 2, icon); tx += 20; }
        gfx_text(g, tx, (w->r.h - FONT_UI_H) / 2, txt, -1, FONT_UI, fg);
    }
    gfx_restore(g, &st);
}

static void dd_set_sel(dropdown_t *d, int idx, bool notify)
{
    if (idx >= d->count) idx = d->count - 1;
    if (idx < 0) idx = d->count ? 0 : -1;
    bool changed = idx != d->sel;
    d->sel = idx;
    ui_invalidate(&d->w);
    if (changed && notify) ui_notify(&d->w, UI_SELECT);
}

static void pop_ensure_visible(void)
{
    if (s_pop.hot < s_pop.sb.pos) s_pop.sb.pos = s_pop.hot;
    if (s_pop.hot >= s_pop.sb.pos + s_pop.sb.page) s_pop.sb.pos = s_pop.hot - s_pop.sb.page + 1;
    scroll_clamp(&s_pop.sb);
}

static void pop_open(dropdown_t *d)
{
    if (s_pop.dd) ui_popup_close();
    if (d->count <= 0) return;
    rect_t c;
    wm_client_rect(d->w.win, &c);
    int x = c.x + d->w.r.x, top = c.y + d->w.r.y, bottom = top + d->w.r.h;
    // below the field if it fits, otherwise on whichever side has more room
    int below = (DESK_H - bottom - 2) / LV_ROW_H, above = (top - 2) / LV_ROW_H;
    int rows = d->count < DD_MAX_ROWS ? d->count : DD_MAX_ROWS;
    bool up = rows > below && above > below;
    int room = up ? above : below;
    if (rows > room) rows = room > 1 ? room : 1;
    int h = rows * LV_ROW_H + 2;
    s_pop.dd = d;
    s_pop.r = rect_make(x, up ? top - h : bottom, d->w.r.w, h);
    s_pop.hot = d->sel < 0 ? 0 : d->sel;
    s_pop.sb = (scroll_t){0, rows, d->count, 0, 0};
    s_pop.sb_drag = false;
    pop_ensure_visible();
    comp_invalidate_rect(&s_pop.r);
    ui_invalidate(&d->w);
}

bool ui_popup_is_open(void) { return s_pop.dd != NULL; }

void ui_popup_close(void)
{
    dropdown_t *d = s_pop.dd;
    if (!d) return;
    s_pop.dd = NULL;
    comp_invalidate_rect(&s_pop.r);
    if (wm_valid(d->w.win)) ui_invalidate(&d->w);
}

static void pop_commit(void)
{
    dropdown_t *d = s_pop.dd;
    int idx = s_pop.hot;
    ui_popup_close();
    if (d && wm_valid(d->w.win)) dd_set_sel(d, idx, true);
}

static bool pop_has_sb(void) { return s_pop.sb.total > s_pop.sb.page; }
static rect_t pop_sb_rect(void)
{
    return rect_make(s_pop.r.x + s_pop.r.w - 1 - SCROLL_W, s_pop.r.y + 1, SCROLL_W, s_pop.r.h - 2);
}

void ui_popup_paint(gfx_t *g, const rect_t *area)
{
    dropdown_t *d = s_pop.dd;
    if (!d || !rect_overlap(&s_pop.r, area)) return;
    rect_t r = s_pop.r;
    gfx_fill(g, r.x, r.y, r.w, r.h, C_FIELD);
    gfx_frame(g, r.x, r.y, r.w, r.h, C_BLACK);
    int inner_w = r.w - 2 - (pop_has_sb() ? SCROLL_W : 0);
    gfx_state_t st;
    gfx_save(g, &st);
    if (gfx_clip(g, r.x + 1, r.y + 1, inner_w, r.h - 2)) {
        for (int i = 0; i < s_pop.sb.page; i++) {
            int idx = s_pop.sb.pos + i;
            if (idx >= d->count) break;
            int y = r.y + 1 + i * LV_ROW_H;
            const bitmap_t *icon;
            const char *txt = dd_text(d, idx, &icon);
            uint8_t fg = C_TEXT;
            if (idx == s_pop.hot) { gfx_fill(g, r.x + 1, y, inner_w, LV_ROW_H, C_SEL); fg = C_SEL_TEXT; }
            int tx = r.x + 5;
            if (icon) { gfx_bitmap(g, tx, y + 1, icon); tx += 20; }
            gfx_text(g, tx, y + 3, txt, -1, FONT_UI, fg);
        }
    }
    gfx_restore(g, &st);
    if (pop_has_sb()) { rect_t sr = pop_sb_rect(); scroll_paint(g, &sr, &s_pop.sb, false); }
}

bool ui_popup_key(const event_t *e)
{
    dropdown_t *d = s_pop.dd;
    if (!d) return false;
    if (!e->down) return true;
    int old_hot = s_pop.hot, old_pos = s_pop.sb.pos;
    bool alt = e->mods & MOD_ALT;
    switch (e->key) {
    case K_UP: if (alt) { pop_commit(); return true; } s_pop.hot--; break;
    case K_DOWN: if (alt) { pop_commit(); return true; } s_pop.hot++; break;
    case K_HOME: s_pop.hot = 0; break;
    case K_END: s_pop.hot = d->count - 1; break;
    case K_PGUP: s_pop.hot -= s_pop.sb.page - 1; break;
    case K_PGDN: s_pop.hot += s_pop.sb.page - 1; break;
    case K_ENTER: case K_KP_ENTER: case K_F4: pop_commit(); return true;
    case K_ESC: ui_popup_close(); return true;
    case K_TAB: pop_commit(); return false;     // the window still moves the focus on
    default:
        if (e->ch > ' ' && e->ch < 127 && !(e->mods & (MOD_CTRL | MOD_ALT))) {
            int idx = dd_find(d, e->ch, s_pop.hot);
            if (idx >= 0) s_pop.hot = idx;
        }
    }
    if (s_pop.hot < 0) s_pop.hot = 0;
    if (s_pop.hot >= d->count) s_pop.hot = d->count - 1;
    pop_ensure_visible();
    if (s_pop.hot != old_hot || s_pop.sb.pos != old_pos) comp_invalidate_rect(&s_pop.r);
    return true;
}

bool ui_popup_mouse(int type, int x, int y, int btn)
{
    dropdown_t *d = s_pop.dd;
    if (!d) return false;
    if (!wm_valid(d->w.win) || (d->w.win->flags & (WIN_DEAD | WIN_HIDDEN | WIN_MINIMIZED))) { ui_popup_close(); return false; }
    if (type == EV_WHEEL) {
        s_pop.sb.pos -= btn * 3;
        scroll_clamp(&s_pop.sb);
        comp_invalidate_rect(&s_pop.r);
        return true;
    }
    rect_t sr = pop_sb_rect();
    if (s_pop.sb_drag || (pop_has_sb() && type == EV_MOUSE_DOWN && btn == BTN_LEFT && rect_has(&sr, x, y))) {
        if (type == EV_MOUSE_DOWN) s_pop.sb_drag = true;
        if (scroll_mouse(&s_pop.sb, &sr, false, type, x, y, 1)) comp_invalidate_rect(&s_pop.r);
        if (type == EV_MOUSE_UP) s_pop.sb_drag = false;
        return true;
    }
    if (rect_has(&s_pop.r, x, y)) {
        int row = (y - s_pop.r.y - 1) / LV_ROW_H;
        if (row < 0) row = 0;
        if (row >= s_pop.sb.page) row = s_pop.sb.page - 1;
        int idx = s_pop.sb.pos + row;
        if (idx >= d->count) idx = d->count - 1;
        if (idx != s_pop.hot) { s_pop.hot = idx; comp_invalidate_rect(&s_pop.r); }
        if (type == EV_MOUSE_UP && btn == BTN_LEFT) pop_commit();
        return true;
    }
    if (type == EV_MOUSE_DOWN) {
        // a press on the field itself only closes the list; anywhere else it also acts on what is there
        rect_t c;
        wm_client_rect(d->w.win, &c);
        rect_t fr = rect_make(c.x + d->w.r.x, c.y + d->w.r.y, d->w.r.w, d->w.r.h);
        ui_popup_close();
        return rect_has(&fr, x, y);
    }
    return type == EV_MOUSE_UP;     // e.g. the release of the press that opened it
}

static bool dd_key(widget_t *w, const event_t *e)
{
    dropdown_t *d = (dropdown_t *)w;
    if (e->mods & MOD_CTRL) return false;
    bool alt = e->mods & MOD_ALT;
    if (e->key == K_F4 && !alt) { pop_open(d); return true; }
    if (alt && (e->key == K_DOWN || e->key == K_UP)) { pop_open(d); return true; }
    if (alt) return false;
    switch (e->key) {
    case K_UP: case K_LEFT: dd_set_sel(d, d->sel - 1, true); return true;
    case K_DOWN: case K_RIGHT: dd_set_sel(d, d->sel + 1, true); return true;
    case K_HOME: dd_set_sel(d, 0, true); return true;
    case K_END: dd_set_sel(d, d->count - 1, true); return true;
    case K_SPACE: pop_open(d); return true;
    }
    if (e->ch > ' ' && e->ch < 127) {
        int idx = dd_find(d, e->ch, d->sel);
        if (idx >= 0) dd_set_sel(d, idx, true);
        return true;
    }
    return false;
}

static void dd_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    if (type != EV_MOUSE_DOWN || btn != BTN_LEFT) return;
    pop_open((dropdown_t *)w);
    // the moves and the release go to the list, so press, drag to an item and release picks it
    ui_capture(NULL);
}

static void dd_destroy(widget_t *w) { if (s_pop.dd == (dropdown_t *)w) ui_popup_close(); }

static const widget_class_t dd_class = {"dropdown", dd_paint, dd_key, dd_mouse, NULL, dd_destroy};

widget_t *ui_dropdown(window_t *win, int x, int y, int w, lv_get_fn get)
{
    dropdown_t *d = ui_new(win, &dd_class, sizeof(dropdown_t), x, y, w, 22);
    if (!d) return NULL;
    d->get = get;
    d->sel = -1;
    d->w.flags = WF_TABSTOP | WF_WANT_CHARS | WF_WANT_ARROWS;
    return &d->w;
}

void ui_dropdown_set_count(widget_t *w, int count)
{
    dropdown_t *d = (dropdown_t *)w;
    if (s_pop.dd == d) ui_popup_close();
    d->count = count;
    if (d->sel >= count) d->sel = count - 1;
    if (d->sel < 0 && count > 0) d->sel = 0;
    ui_invalidate(w);
}

int ui_dropdown_sel(const widget_t *w) { return ((const dropdown_t *)w)->sel; }
void ui_dropdown_select(widget_t *w, int index) { dd_set_sel((dropdown_t *)w, index, false); }

// ---------------------------------------------------------------- canvas

typedef struct { widget_t w; const canvas_ops_t *ops; } canvas_t;

static void canvas_paint(widget_t *w, gfx_t *g) { canvas_t *c = (canvas_t *)w; if (c->ops->paint) c->ops->paint(w, g); }
static bool canvas_key(widget_t *w, const event_t *e) { canvas_t *c = (canvas_t *)w; return c->ops->key ? c->ops->key(w, e) : false; }
static void canvas_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    canvas_t *c = (canvas_t *)w;
    if (c->ops->mouse) c->ops->mouse(w, type, x, y, btn, clicks);
}
static const widget_class_t canvas_class = {"canvas", canvas_paint, canvas_key, canvas_mouse, NULL, NULL};

widget_t *ui_canvas(window_t *win, int x, int y, int w, int h, const canvas_ops_t *ops, bool focusable)
{
    canvas_t *c = ui_new(win, &canvas_class, sizeof(canvas_t), x, y, w, h);
    if (!c) return NULL;
    c->ops = ops;
    if (focusable) c->w.flags = WF_TABSTOP | WF_WANT_ARROWS | WF_WANT_KEYUP;
    return &c->w;
}

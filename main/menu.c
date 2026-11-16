// Popup menus and window menu bars. Fully keyboard-driven (arrows, Enter, Esc, mnemonics) and
// mouse-driven (hover, click, press-drag-release).
#include <string.h>
#include <ctype.h>
#include "wm.h"
#include "comp.h"

#define MAX_LEVELS 4
#define ITEM_H 20
#define SEP_H 8
#define ICON_COL 24

typedef struct { const menu_item_t *items; int n; rect_t r; int sel; bool has_icons; } level_t;
static level_t s_lv[MAX_LEVELS];
static int s_depth;
static menu_cb_t s_cb;
static void *s_ctx;
static window_t *s_bar_win;     // menu bar that has the keyboard (with or without an open dropdown)
static int s_bar_idx;

bool menu_is_open(void) { return s_depth > 0 || s_bar_win; }
window_t *menubar_owner(void) { return s_bar_win; }
int menubar_active_index(void) { return s_bar_win ? s_bar_idx : -1; }

static bool is_sep(const menu_item_t *it) { return it->label[0] == '-' && !it->label[1]; }

static int item_flags(const menu_item_t *it)
{
    int f = it->flags;
    if (s_bar_win && s_bar_win->on_menu_state && it->cmd) f |= s_bar_win->on_menu_state(s_bar_win, it->cmd);
    return f;
}

static void split_label(const char *label, char *text, int tsz, const char **accel)
{
    const char *tab = strchr(label, '\t');
    int n = tab ? (int)(tab - label) : (int)strlen(label);
    if (n >= tsz) n = tsz - 1;
    memcpy(text, label, n);
    text[n] = 0;
    *accel = tab ? tab + 1 : NULL;
}

static void inval_level(int i) { comp_invalidate_rect(&s_lv[i].r); }

static void bar_rect(const window_t *w, rect_t *r)
{
    *r = rect_make(w->r.x + BORDER_W, w->r.y + BORDER_W + TITLE_H + 1, w->r.w - 2 * BORDER_W, MENUBAR_H);
}

static bool bar_item_rect(const window_t *w, int idx, rect_t *out)
{
    rect_t bar;
    bar_rect(w, &bar);
    int x = bar.x + 1;
    for (int i = 0; w->menubar[i].label; i++) {
        int iw = gfx_label_w(w->menubar[i].label, FONT_UI) + 14;
        if (i == idx) { *out = rect_make(x, bar.y, iw, MENUBAR_H - 1); return true; }
        x += iw;
    }
    return false;
}

static int bar_count(const window_t *w)
{
    int n = 0;
    while (w->menubar[n].label) n++;
    return n;
}

static void inval_bar(void)
{
    if (!s_bar_win) return;
    rect_t bar;
    bar_rect(s_bar_win, &bar);
    comp_invalidate_rect(&bar);
}

static void open_level(const menu_item_t *items, int x, int y, bool above, int alt_x)
{
    if (s_depth >= MAX_LEVELS) return;
    level_t *l = &s_lv[s_depth];
    l->items = items;
    l->sel = -1;
    l->has_icons = false;
    int w = 60, h = 6, n = 0;
    for (const menu_item_t *it = items; it->label; it++, n++) {
        if (is_sep(it)) { h += SEP_H; continue; }
        char text[64];
        const char *accel;
        split_label(it->label, text, sizeof(text), &accel);
        int iw = gfx_label_w(text, FONT_UI) + 16 + (accel ? gfx_text_w(accel, -1, FONT_UI) + 24 : 0) + (it->sub ? 14 : 0);
        if (iw > w) w = iw;
        if (it->icon) l->has_icons = true;
        h += ITEM_H;
    }
    w += l->has_icons ? ICON_COL : 18;
    w += 6;
    l->n = n;
    if (above) y -= h;
    if (x + w > SCREEN_W) x = alt_x >= 0 ? alt_x - w : SCREEN_W - w;
    if (x < 0) x = 0;
    if (y + h > SCREEN_H) y = SCREEN_H - h;
    if (y < 0) y = 0;
    l->r = rect_make(x, y, w, h);
    s_depth++;
    inval_level(s_depth - 1);
}

static void close_to(int depth)
{
    bool was = s_depth > 0;
    while (s_depth > depth) { s_depth--; inval_level(s_depth); }
    if (was && s_depth == 0) shell_windows_changed(); // the menu button pops back up
}

void menu_close_all(void)
{
    close_to(0);
    inval_bar();
    s_bar_win = NULL;
}

void menu_popup(const menu_item_t *items, int x, int y, bool above, menu_cb_t cb, void *ctx)
{
    menu_close_all();
    s_cb = cb;
    s_ctx = ctx;
    open_level(items, x, y, above, -1);
    // preselect the first usable item so the keyboard has a visible position
    level_t *l = &s_lv[0];
    for (int i = 0; i < l->n; i++) if (!is_sep(&l->items[i])) { l->sel = i; break; }
}

static int item_y(const level_t *l, int idx)
{
    int y = l->r.y + 3;
    for (int i = 0; i < idx; i++) y += is_sep(&l->items[i]) ? SEP_H : ITEM_H;
    return y;
}

static int item_at(const level_t *l, int y)
{
    int iy = l->r.y + 3;
    for (int i = 0; i < l->n; i++) {
        int h = is_sep(&l->items[i]) ? SEP_H : ITEM_H;
        if (y >= iy && y < iy + h) return is_sep(&l->items[i]) ? -1 : i;
        iy += h;
    }
    return -1;
}

static void open_sub(int level)
{
    level_t *l = &s_lv[level];
    if (l->sel < 0 || !l->items[l->sel].sub) return;
    close_to(level + 1);
    open_level(l->items[l->sel].sub, l->r.x + l->r.w - 3, item_y(l, l->sel) - 3, false, l->r.x + 3);
}

static void menubar_open(window_t *w, int idx, bool select_first)
{
    close_to(0);
    if (s_bar_win != w) { s_bar_win = w; }
    s_bar_idx = idx;
    inval_bar();
    rect_t ir;
    if (!bar_item_rect(w, idx, &ir) || !w->menubar[idx].sub) return;
    open_level(w->menubar[idx].sub, ir.x, ir.y + ir.h, false, -1);
    if (select_first) {
        level_t *l = &s_lv[0];
        for (int i = 0; i < l->n; i++) if (!is_sep(&l->items[i])) { l->sel = i; break; }
    }
}

static void bar_command(void *ctx, int cmd)
{
    window_t *w = ctx;
    if (cmd > 0 && wm_valid(w) && w->on_command) w->on_command(w, cmd);
}

static void activate(int level)
{
    level_t *l = &s_lv[level];
    if (l->sel < 0) return;
    const menu_item_t *it = &l->items[l->sel];
    if (item_flags(it) & MI_DISABLED) return;
    if (it->sub) {
        open_sub(level);
        level_t *n = &s_lv[s_depth - 1];
        for (int i = 0; i < n->n; i++) if (!is_sep(&n->items[i])) { n->sel = i; break; }
        inval_level(s_depth - 1);
        return;
    }
    int cmd = it->cmd;
    menu_cb_t cb = s_bar_win ? bar_command : s_cb;
    void *ctx = s_bar_win ? (void *)s_bar_win : s_ctx;
    menu_close_all();
    if (cb) cb(ctx, cmd);
}

static void move_sel(level_t *l, int dir)
{
    if (!l->n) return;
    int i = l->sel;
    for (int k = 0; k < l->n; k++) {
        i += dir;
        if (i < 0) i = l->n - 1;
        if (i >= l->n) i = 0;
        if (!is_sep(&l->items[i])) { l->sel = i; return; }
    }
}

bool menu_key(const event_t *e)
{
    if (!e->down) return true;
    bool alt_key = e->key == K_LALT || e->key == K_RALT;
    if (s_depth == 0) {
        // menu bar has the keyboard, nothing dropped down
        window_t *w = s_bar_win;
        int n = bar_count(w);
        if (e->key == K_LEFT) { s_bar_idx = (s_bar_idx + n - 1) % n; inval_bar(); }
        else if (e->key == K_RIGHT) { s_bar_idx = (s_bar_idx + 1) % n; inval_bar(); }
        else if (e->key == K_DOWN || e->key == K_UP || e->key == K_ENTER || e->key == K_KP_ENTER || e->key == K_SPACE) menubar_open(w, s_bar_idx, true);
        else if (e->key == K_ESC || e->key == K_F10 || (alt_key && !e->repeat)) menu_close_all();
        else if (e->ch > ' ') {
            for (int i = 0; i < n; i++) {
                if (gfx_label_mnemonic(w->menubar[i].label) == tolower((unsigned char)e->ch)) { menubar_open(w, i, true); break; }
            }
        }
        return true;
    }
    level_t *l = &s_lv[s_depth - 1];
    inval_level(s_depth - 1);
    switch (e->key) {
    case K_UP: move_sel(l, -1); break;
    case K_DOWN: move_sel(l, 1); break;
    case K_HOME: l->sel = -1; move_sel(l, 1); break;
    case K_END: l->sel = l->n; move_sel(l, -1); break;
    case K_RIGHT:
        if (l->sel >= 0 && l->items[l->sel].sub && !(item_flags(&l->items[l->sel]) & MI_DISABLED)) activate(s_depth - 1);
        else if (s_bar_win) menubar_open(s_bar_win, (s_bar_idx + 1) % bar_count(s_bar_win), true);
        break;
    case K_LEFT:
        if (s_depth > 1) close_to(s_depth - 1);
        else if (s_bar_win) menubar_open(s_bar_win, (s_bar_idx + bar_count(s_bar_win) - 1) % bar_count(s_bar_win), true);
        break;
    case K_ENTER: case K_KP_ENTER: case K_SPACE:
        activate(s_depth - 1);
        break;
    case K_ESC:
        if (s_depth > 1 || s_bar_win) close_to(s_depth - 1);
        else {
            menu_cb_t cb = s_cb; void *ctx = s_ctx;
            menu_close_all();
            if (cb) cb(ctx, -1); // cancelled from the keyboard
        }
        break;
    case K_F10:
        menu_close_all();
        break;
    default:
        if (alt_key && !e->repeat) { menu_close_all(); break; }
        if (e->ch > ' ') {
            char c = tolower((unsigned char)e->ch);
            int found = -1;
            for (int i = 0; i < l->n && found < 0; i++) {
                if (!is_sep(&l->items[i]) && gfx_label_mnemonic(l->items[i].label) == c) found = i;
            }
            for (int k = 1; k <= l->n && found < 0; k++) {
                int i = (l->sel + k + l->n) % l->n;
                if (!is_sep(&l->items[i]) && tolower((unsigned char)l->items[i].label[0]) == c) found = i;
            }
            if (found >= 0) { l->sel = found; if (gfx_label_mnemonic(l->items[found].label) == c) activate(s_depth - 1); }
        }
    }
    if (s_depth > 0) inval_level(s_depth - 1);
    return true;
}

bool menu_mouse(int type, int x, int y, int btn)
{
    int lvl = -1;
    for (int i = s_depth - 1; i >= 0; i--) if (rect_has(&s_lv[i].r, x, y)) { lvl = i; break; }
    if (lvl >= 0) {
        level_t *l = &s_lv[lvl];
        int idx = item_at(l, y);
        if (type == EV_MOUSE_MOVE || type == EV_MOUSE_DOWN) {
            if (idx >= 0 && idx != l->sel) {
                l->sel = idx;
                close_to(lvl + 1);
                inval_level(lvl);
                if (l->items[idx].sub && !(item_flags(&l->items[idx]) & MI_DISABLED)) open_sub(lvl);
            }
        } else if (type == EV_MOUSE_UP && btn == BTN_LEFT) {
            if (idx >= 0) {
                l->sel = idx;
                if (l->items[idx].sub) { if (s_depth == lvl + 1) open_sub(lvl); }
                else activate(lvl);
            }
        }
        return true;
    }
    // outside the popups
    if (s_bar_win) {
        rect_t bar;
        bar_rect(s_bar_win, &bar);
        if (rect_has(&bar, x, y)) {
            for (int i = 0; s_bar_win->menubar[i].label; i++) {
                rect_t ir;
                bar_item_rect(s_bar_win, i, &ir);
                if (!rect_has(&ir, x, y)) continue;
                if (type == EV_MOUSE_DOWN) {
                    if (i == s_bar_idx && s_depth) menu_close_all(); else menubar_open(s_bar_win, i, false);
                } else if (type == EV_MOUSE_MOVE && i != s_bar_idx && s_depth) {
                    menubar_open(s_bar_win, i, false);
                }
                return true;
            }
            if (type == EV_MOUSE_DOWN) menu_close_all();
            return true;
        }
    }
    if (type == EV_MOUSE_DOWN) {
        menu_close_all();
        return false; // the click also acts on whatever is under it
    }
    return type == EV_MOUSE_UP;
}

static const uint16_t g_sub_arrow[7] = {0x8000, 0xC000, 0xE000, 0xF000, 0xE000, 0xC000, 0x8000};
static const uint16_t g_check[7] = {0x0200, 0x0600, 0x8E00, 0xDC00, 0xF800, 0x7000, 0x2000};
static const uint16_t g_dot[5] = {0x7000, 0xF800, 0xF800, 0xF800, 0x7000};

void menu_paint(gfx_t *g, const rect_t *area)
{
    for (int li = 0; li < s_depth; li++) {
        level_t *l = &s_lv[li];
        if (!rect_overlap(&l->r, area)) continue;
        gfx_fill(g, l->r.x, l->r.y, l->r.w, l->r.h, C_FACE);
        ui_draw_raised(g, l->r.x, l->r.y, l->r.w, l->r.h);
        int y = l->r.y + 3;
        int tx = l->r.x + 3 + (l->has_icons ? ICON_COL : 18);
        for (int i = 0; i < l->n; i++) {
            const menu_item_t *it = &l->items[i];
            if (is_sep(it)) {
                gfx_hline(g, l->r.x + 4, y + 3, l->r.w - 8, C_SHADOW);
                gfx_hline(g, l->r.x + 4, y + 4, l->r.w - 8, C_HILIGHT);
                y += SEP_H;
                continue;
            }
            int fl = item_flags(it);
            bool sel = i == l->sel, dis = fl & MI_DISABLED;
            uint8_t fg = dis ? C_TEXT_DIS : C_TEXT;
            if (sel) { gfx_fill(g, l->r.x + 3, y, l->r.w - 6, ITEM_H, C_SEL); fg = dis ? C_GREY : C_SEL_TEXT; }
            char text[64];
            const char *accel;
            split_label(it->label, text, sizeof(text), &accel);
            if (it->icon) gfx_bitmap(g, l->r.x + 6, y + 2, it->icon);
            else if (fl & MI_CHECKED) gfx_glyph16(g, l->r.x + 8, y + 7, g_check, 7, 7, fg);
            else if (fl & MI_RADIO) gfx_glyph16(g, l->r.x + 9, y + 8, g_dot, 5, 5, fg);
            if (dis && !sel) gfx_label(g, tx + 1, y + 5, text, FONT_UI, C_HILIGHT, true);
            gfx_label(g, tx, y + 4, text, FONT_UI, fg, true);
            if (accel) gfx_text(g, l->r.x + l->r.w - 10 - gfx_text_w(accel, -1, FONT_UI) - (it->sub ? 12 : 0), y + 4, accel, -1, FONT_UI, fg);
            if (it->sub) gfx_glyph16(g, l->r.x + l->r.w - 12, y + 7, g_sub_arrow, 4, 7, fg);
            y += ITEM_H;
        }
    }
}

// ---------------------------------------------------------------- menu bar

void menubar_paint(window_t *w, gfx_t *g, const rect_t *bar)
{
    gfx_hline(g, bar->x, bar->y + bar->h - 1, bar->w, C_SHADOW);
    for (int i = 0; w->menubar[i].label; i++) {
        rect_t ir;
        bar_item_rect(w, i, &ir);
        bool sel = s_bar_win == w && s_bar_idx == i;
        if (sel) gfx_fill(g, ir.x, ir.y + 1, ir.w, ir.h - 2, C_SEL);
        gfx_label(g, ir.x + 7, ir.y + 3, w->menubar[i].label, FONT_UI, sel ? C_SEL_TEXT : C_TEXT, true);
    }
}

bool menubar_key(window_t *w, const event_t *e)
{
    if (!w->menubar) return false;
    if (wm_alt_tapped(e) || (e->down && e->key == K_F10 && !(e->mods & (MOD_CTRL | MOD_ALT | MOD_SHIFT)))) {
        s_bar_win = w;
        s_bar_idx = 0;
        s_cb = NULL;
        inval_bar();
        if (w->focus) ui_invalidate(w->focus);
        return true;
    }
    if (e->down && (e->mods & MOD_ALT) && !(e->mods & MOD_CTRL) && e->ch > ' ') {
        for (int i = 0; w->menubar[i].label; i++) {
            if (gfx_label_mnemonic(w->menubar[i].label) == tolower((unsigned char)e->ch)) { menubar_open(w, i, true); return true; }
        }
    }
    return false;
}

void menubar_click(window_t *w, int x, int y)
{
    for (int i = 0; w->menubar[i].label; i++) {
        rect_t ir;
        bar_item_rect(w, i, &ir);
        if (rect_has(&ir, x, y)) { menubar_open(w, i, false); return; }
    }
}

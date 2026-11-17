// Text editor: a gap-buffer text area widget (text lives in PSRAM) and the editor window around it.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include "wm.h"
#include "apps.h"
#include "os.h"
#include "fsutil.h"
#include "esp_heap_caps.h"

#define CH_W 8
#define CH_H 16
#define TAB_W 4

typedef struct {
    widget_t w;
    char *buf;
    int cap, gap0, gap1;        // text = buf[0..gap0) + buf[gap1..cap)
    int cur, anchor;            // caret and selection anchor (logical offsets)
    int top_line, top_off;      // first visible line and where it starts
    int nlines;
    int hscroll;                // in columns
    int want_col;               // column to keep while moving up/down (-1 = take the current one)
    scroll_t vsb;
    bool modified, sb_drag, selecting;
    void (*on_change)(widget_t *w);
} textarea_t;

static inline int ta_len(const textarea_t *t) { return t->cap - (t->gap1 - t->gap0); }
static inline char ta_ch(const textarea_t *t, int i) { return i < t->gap0 ? t->buf[i] : t->buf[i + (t->gap1 - t->gap0)]; }

// the bytes of the glyph just before a caret, and just after it: an accented letter is 2
// bytes, so Backspace and Delete take a whole letter and not half of one
static int ta_back_span(const textarea_t *t, int pos)
{
    if (pos >= 2 && (ta_ch(t, pos - 2) & 0xC0) == 0xC0) return 2;
    return 1;
}

static int ta_fwd_span(const textarea_t *t, int pos, int len)
{
    unsigned char c = pos < len ? (unsigned char)ta_ch(t, pos) : 0;
    if ((c & 0xE0) == 0xC0 && pos + 1 < len) return 2;
    return 1;
}

static void ta_move_gap(textarea_t *t, int pos)
{
    if (pos < t->gap0) {
        int n = t->gap0 - pos;
        memmove(t->buf + t->gap1 - n, t->buf + pos, n);
        t->gap0 -= n; t->gap1 -= n;
    } else if (pos > t->gap0) {
        int n = pos - t->gap0;
        memmove(t->buf + t->gap0, t->buf + t->gap1, n);
        t->gap0 += n; t->gap1 += n;
    }
}

static bool ta_grow(textarea_t *t, int need)
{
    if (t->gap1 - t->gap0 >= need) return true;
    int len = ta_len(t);
    int ncap = len + need + 4096 + len / 4;
    char *nb = heap_caps_malloc(ncap, MALLOC_CAP_SPIRAM);
    if (!nb) return false;
    memcpy(nb, t->buf, t->gap0);
    int tail = t->cap - t->gap1;
    memcpy(nb + ncap - tail, t->buf + t->gap1, tail);
    free(t->buf);
    t->buf = nb;
    t->gap1 = ncap - tail;
    t->cap = ncap;
    return true;
}

static int count_nl(const char *s, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) if (s[i] == '\n') c++;
    return c;
}

static int ta_count_nl(const textarea_t *t, int a, int b)
{
    int c = 0;
    for (int i = a; i < b; i++) if (ta_ch(t, i) == '\n') c++;
    return c;
}

static int ta_line_start(const textarea_t *t, int off)
{
    while (off > 0 && ta_ch(t, off - 1) != '\n') off--;
    return off;
}

static int ta_line_end(const textarea_t *t, int off)
{
    int len = ta_len(t);
    while (off < len && ta_ch(t, off) != '\n') off++;
    return off;
}

// visual column of offset 'off' on the line starting at 'ls': one column per glyph, so an
// accented letter (2 bytes) takes the room of one letter
static int ta_vcol(const textarea_t *t, int ls, int off)
{
    int col = 0, len = ta_len(t), i = ls;
    while (i < off) {
        int n;
        gfx_glyph(t->buf, i, len, &n);
        if (ta_ch(t, i) == '\t') { col += TAB_W - (col % TAB_W); i++; }
        else { col++; i += n; }
    }
    return col;
}

// offset on the line starting at 'ls' closest to visual column 'col'
static int ta_off_at_col(const textarea_t *t, int ls, int col)
{
    int len = ta_len(t), c = 0, i = ls;
    while (i < len && ta_ch(t, i) != '\n') {
        int n;
        gfx_glyph(t->buf, i, len, &n);
        int wdt = ta_ch(t, i) == '\t' ? TAB_W - (c % TAB_W) : 1;
        if (c + wdt > col) { if (col - c > wdt / 2) i += n; break; }
        c += wdt; i += n;
    }
    return i;
}

static int ta_rows(const textarea_t *t) { int r = (t->w.r.h - 4) / CH_H; return r > 0 ? r : 1; }
static int ta_cols(const textarea_t *t) { int c = (t->w.r.w - 4 - SCROLL_W - 4) / CH_W; return c > 0 ? c : 1; }

static void ta_set_top(textarea_t *t, int line)
{
    int max = t->nlines - 1;
    if (line > max) line = max;
    if (line < 0) line = 0;
    int len = ta_len(t);
    if (line < t->top_line / 2) { t->top_line = 0; t->top_off = 0; }
    while (t->top_line < line) {
        int e = ta_line_end(t, t->top_off);
        if (e >= len) break;
        t->top_off = e + 1; t->top_line++;
    }
    while (t->top_line > line && t->top_off > 0) {
        t->top_off = ta_line_start(t, t->top_off - 1);
        t->top_line--;
    }
}

static void ta_reveal(textarea_t *t)
{
    int rows = ta_rows(t), cols = ta_cols(t);
    int ls = ta_line_start(t, t->cur);
    if (t->cur < t->top_off) {
        t->top_line -= ta_count_nl(t, ls, t->top_off);
        t->top_off = ls;
    } else {
        int line = t->top_line + ta_count_nl(t, t->top_off, t->cur);
        if (line >= t->top_line + rows) ta_set_top(t, line - rows + 1);
    }
    int col = ta_vcol(t, ls, t->cur);
    if (col < t->hscroll) t->hscroll = col > 8 ? col - 8 : 0;
    if (col >= t->hscroll + cols) t->hscroll = col - cols + 1;
}

static void ta_changed(textarea_t *t)
{
    t->modified = true;
    if (t->on_change) t->on_change(&t->w);
}

static bool ta_insert(textarea_t *t, int pos, const char *s, int n)
{
    if (n <= 0) return true;
    if (!ta_grow(t, n)) return false;
    ta_move_gap(t, pos);
    int w = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') continue;                    // DOS line ends become plain newlines
        t->buf[t->gap0 + w++] = c;
    }
    t->nlines += count_nl(t->buf + t->gap0, w);
    t->gap0 += w;
    if (t->top_off > pos) t->top_off += w;
    return true;
}

static void ta_erase(textarea_t *t, int pos, int n)
{
    if (n <= 0) return;
    ta_move_gap(t, pos);
    t->nlines -= count_nl(t->buf + t->gap1, n);
    t->gap1 += n;
}

static bool ta_has_sel(const textarea_t *t) { return t->cur != t->anchor; }

static void ta_delete_sel(textarea_t *t)
{
    if (!ta_has_sel(t)) return;
    int a = t->cur < t->anchor ? t->cur : t->anchor, b = t->cur < t->anchor ? t->anchor : t->cur;
    if (t->top_off > a) {
        // the view starts inside or after the deleted text: restart it at the line of 'a'
        int ls = ta_line_start(t, a);
        t->top_line -= ta_count_nl(t, ls, t->top_off);
        t->top_off = ls;
    }
    ta_erase(t, a, b - a);
    t->cur = t->anchor = a;
}

static void ta_type(textarea_t *t, const char *s, int n)
{
    ta_delete_sel(t);
    int before = ta_len(t);
    if (ta_insert(t, t->cur, s, n)) t->cur += ta_len(t) - before;
    t->anchor = t->cur;
    t->want_col = -1;
    ta_changed(t);
}

static char *ta_copy_range(const textarea_t *t, int a, int b)
{
    char *s = heap_caps_malloc(b - a + 1, MALLOC_CAP_SPIRAM);
    if (!s) return NULL;
    for (int i = a; i < b; i++) s[i - a] = ta_ch(t, i);
    s[b - a] = 0;
    return s;
}

static void ta_copy(textarea_t *t)
{
    if (!ta_has_sel(t)) return;
    int a = t->cur < t->anchor ? t->cur : t->anchor, b = t->cur < t->anchor ? t->anchor : t->cur;
    char *s = ta_copy_range(t, a, b);
    if (s) { clip_set(s, b - a); free(s); }
}

static bool is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void ta_paint(widget_t *w, gfx_t *g)
{
    textarea_t *t = (textarea_t *)w;
    int rows = ta_rows(t), cols = ta_cols(t) + 1;
    gfx_fill(g, 0, 0, w->r.w, w->r.h, C_FIELD);
    ui_draw_sunken(g, 0, 0, w->r.w, w->r.h);
    t->vsb.total = t->nlines; t->vsb.page = rows; t->vsb.pos = t->top_line;
    rect_t sr = {w->r.w - 2 - SCROLL_W, 2, SCROLL_W, w->r.h - 4};
    scroll_paint(g, &sr, &t->vsb, false);
    gfx_state_t st;
    gfx_save(g, &st);
    if (!gfx_clip(g, 2, 2, w->r.w - 4 - SCROLL_W, w->r.h - 4)) { gfx_restore(g, &st); return; }
    bool foc = ui_focused(w);
    int s0 = t->cur < t->anchor ? t->cur : t->anchor, s1 = t->cur < t->anchor ? t->anchor : t->cur;
    int len = ta_len(t), off = t->top_off;
    int x0 = 4;
    for (int row = 0; row <= rows && off <= len; row++) {
        int y = 2 + row * CH_H;
        bool vis = gfx_visible(g, 0, y, w->r.w, CH_H);
        int col = 0, i = off;
        char run[256];
        int rb = 0, rg = 0, rcol = 0;   // the run of text: bytes and glyphs
        bool rsel = false;
        for (;;) {
            bool end = i >= len || ta_ch(t, i) == '\n';
            int n = 1;
            if (!end) gfx_glyph(t->buf, i, len, &n);   // a glyph is 1 byte, or 2 for an accent
            char c = end ? ' ' : ta_ch(t, i);
            int wdt = c == '\t' ? TAB_W - (col % TAB_W) : 1;
            bool sel = i >= s0 && i < s1;
            if (i == t->cur && foc && ui_caret_on() && vis) gfx_fill(g, x0 + (col - t->hscroll) * CH_W - 1, y, 2, CH_H, C_BLACK);
            if (end && !sel) break;
            if (vis) for (int k = 0; k < wdt; k++) {
                int vc = col + k - t->hscroll;
                if (vc < 0 || vc >= cols) continue;
                if (rb && (sel != rsel || rb + n > (int)sizeof(run))) {
                    if (rsel) gfx_fill(g, x0 + rcol * CH_W, y, rg * CH_W, CH_H, foc ? C_SEL : C_GREY);
                    gfx_text(g, x0 + rcol * CH_W, y, run, rb, FONT_MONO, rsel && foc ? C_SEL_TEXT : C_TEXT);
                    rb = 0; rg = 0;
                }
                if (!rb) { rcol = vc; rsel = sel; }
                if (end) run[rb++] = ' ';
                else for (int b = 0; b < n; b++) run[rb++] = ta_ch(t, i + b) == '\t' ? ' ' : ta_ch(t, i + b);
                rg++;
            }
            col += wdt;
            if (end) break;
            i += n;
            if (col - t->hscroll > cols && i >= s1 && !(i < t->cur)) { i = ta_line_end(t, i); break; }
        }
        if (rb && vis) {
            if (rsel) gfx_fill(g, x0 + rcol * CH_W, y, rg * CH_W, CH_H, foc ? C_SEL : C_GREY);
            gfx_text(g, x0 + rcol * CH_W, y, run, rb, FONT_MONO, rsel && foc ? C_SEL_TEXT : C_TEXT);
        }
        if (i >= len) break;
        off = i + 1;
    }
    gfx_restore(g, &st);
}

static void ta_move(textarea_t *t, int to, bool select, bool keep_col)
{
    int len = ta_len(t);
    if (to < 0) to = 0;
    if (to > len) to = len;
    t->cur = to;
    if (!select) t->anchor = to;
    if (!keep_col) t->want_col = -1;
    ta_reveal(t);
    ui_invalidate(&t->w);
    if (t->on_change) t->on_change(&t->w);
}

static void ta_vertical(textarea_t *t, int dlines, bool select)
{
    int ls = ta_line_start(t, t->cur);
    if (t->want_col < 0) t->want_col = ta_vcol(t, ls, t->cur);
    int len = ta_len(t);
    while (dlines < 0 && ls > 0) { ls = ta_line_start(t, ls - 1); dlines++; }
    while (dlines > 0) { int e = ta_line_end(t, ls); if (e >= len) break; ls = e + 1; dlines--; }
    ta_move(t, ta_off_at_col(t, ls, t->want_col), select, true);
}

static bool ta_key(widget_t *w, const event_t *e)
{
    textarea_t *t = (textarea_t *)w;
    bool shift = e->mods & MOD_SHIFT, ctrl = e->mods & MOD_CTRL;
    if (e->mods & MOD_ALT) return false;
    int len = ta_len(t);
    switch (e->key) {
    case K_LEFT:
        if (ctrl) { int i = t->cur; while (i > 0 && !is_word(ta_ch(t, i - 1))) i--; while (i > 0 && is_word(ta_ch(t, i - 1))) i--; ta_move(t, i, shift, false); }
        else if (!shift && ta_has_sel(t)) ta_move(t, t->cur < t->anchor ? t->cur : t->anchor, false, false);
        else ta_move(t, t->cur - ta_back_span(t, t->cur), shift, false);
        return true;
    case K_RIGHT:
        if (ctrl) { int i = t->cur; while (i < len && is_word(ta_ch(t, i))) i++; while (i < len && !is_word(ta_ch(t, i))) i++; ta_move(t, i, shift, false); }
        else if (!shift && ta_has_sel(t)) ta_move(t, t->cur > t->anchor ? t->cur : t->anchor, false, false);
        else ta_move(t, t->cur + ta_fwd_span(t, t->cur, len), shift, false);
        return true;
    case K_UP: ta_vertical(t, -1, shift); return true;
    case K_DOWN: ta_vertical(t, 1, shift); return true;
    case K_PGUP: ta_vertical(t, -(ta_rows(t) - 1), shift); return true;
    case K_PGDN: ta_vertical(t, ta_rows(t) - 1, shift); return true;
    case K_HOME: ta_move(t, ctrl ? 0 : ta_line_start(t, t->cur), shift, false); return true;
    case K_END: ta_move(t, ctrl ? len : ta_line_end(t, t->cur), shift, false); return true;
    case K_BKSP:
        if (!ta_has_sel(t)) { if (t->cur == 0) return true; t->anchor = t->cur - ta_back_span(t, t->cur); }
        ta_delete_sel(t); t->want_col = -1; ta_changed(t); ta_reveal(t); ui_invalidate(w);
        return true;
    case K_DEL:
        if (!ta_has_sel(t)) { if (t->cur >= len) return true; t->anchor = t->cur + ta_fwd_span(t, t->cur, len); }
        ta_delete_sel(t); t->want_col = -1; ta_changed(t); ta_reveal(t); ui_invalidate(w);
        return true;
    case K_ENTER: case K_KP_ENTER: {
        // keep the indentation of the current line
        char ind[64] = "\n";
        int n = 1, ls = ta_line_start(t, t->cur);
        for (int i = ls; i < t->cur && n < 62 && (ta_ch(t, i) == ' ' || ta_ch(t, i) == '\t'); i++) ind[n++] = ta_ch(t, i);
        ta_type(t, ind, n);
        ta_reveal(t); ui_invalidate(w);
        return true;
    }
    case K_TAB:
        if (ctrl) return false;
        ta_type(t, "\t", 1); ta_reveal(t); ui_invalidate(w);
        return true;
    }
    if (ctrl) {
        switch (e->key) {
        case K_A: t->anchor = 0; t->cur = len; ui_invalidate(w); if (t->on_change) t->on_change(w); return true;
        case K_C: ta_copy(t); return true;
        case K_X: if (ta_has_sel(t)) { ta_copy(t); ta_delete_sel(t); ta_changed(t); ta_reveal(t); ui_invalidate(w); } return true;
        case K_V: { int n; const char *c = clip_get(&n); if (n) { ta_type(t, c, n); ta_reveal(t); ui_invalidate(w); } return true; }
        }
        return false;
    }
    if (e->chs[0]) {
        ta_type(t, e->chs, (int)strlen(e->chs));
        ta_reveal(t);
        ui_invalidate(w);
        return true;
    }
    return false;
}

static int ta_off_at_point(textarea_t *t, int x, int y)
{
    int row = (y - 2) / CH_H;
    if (row < 0) row = 0;
    int len = ta_len(t), off = t->top_off;
    for (int r = 0; r < row; r++) {
        int e = ta_line_end(t, off);
        if (e >= len) break;
        off = e + 1;
    }
    int col = (x - 4 + CH_W / 2) / CH_W + t->hscroll;
    if (col < 0) col = 0;
    return ta_off_at_col(t, off, col);
}

static void ta_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    textarea_t *t = (textarea_t *)w;
    rect_t sr = {w->r.w - 2 - SCROLL_W, 2, SCROLL_W, w->r.h - 4};
    if (type == EV_WHEEL) { ta_set_top(t, t->top_line - btn * 3); ui_invalidate(w); return; }
    if (t->sb_drag || (type == EV_MOUSE_DOWN && btn == BTN_LEFT && rect_has(&sr, x, y))) {
        if (type == EV_MOUSE_DOWN) t->sb_drag = true;
        t->vsb.total = t->nlines; t->vsb.page = ta_rows(t); t->vsb.pos = t->top_line;
        if (scroll_mouse(&t->vsb, &sr, false, type, x, y, 1)) { ta_set_top(t, t->vsb.pos); ui_invalidate(w); }
        if (type == EV_MOUSE_UP) { t->sb_drag = false; ui_invalidate(w); }
        return;
    }
    if (type == EV_MOUSE_DOWN && btn == BTN_LEFT) {
        int off = ta_off_at_point(t, x, y);
        if (clicks == 2) {
            int a = off, b = off, len = ta_len(t);
            while (a > 0 && is_word(ta_ch(t, a - 1))) a--;
            while (b < len && is_word(ta_ch(t, b))) b++;
            t->anchor = a; t->cur = b;
        } else {
            t->cur = off;
            if (!(input_mods() & MOD_SHIFT)) t->anchor = off;
            t->selecting = true;
        }
        t->want_col = -1;
        ui_invalidate(w);
        if (t->on_change) t->on_change(w);
    } else if (type == EV_MOUSE_MOVE && t->selecting) {
        if (y < 0) ta_set_top(t, t->top_line - 1);
        if (y > w->r.h) ta_set_top(t, t->top_line + 1);
        t->cur = ta_off_at_point(t, x, y < 2 ? 2 : y);
        ui_invalidate(w);
    } else if (type == EV_MOUSE_UP) {
        t->selecting = false;
    }
}

static void ta_destroy(widget_t *w) { free(((textarea_t *)w)->buf); }
static const widget_class_t ta_class = {"text", ta_paint, ta_key, ta_mouse, NULL, ta_destroy};

static void ta_set_text(textarea_t *t, const char *s, int n)
{
    free(t->buf);
    t->cap = 4096; t->gap0 = 0; t->gap1 = t->cap;
    t->buf = heap_caps_malloc(t->cap, MALLOC_CAP_SPIRAM);
    t->cur = t->anchor = 0; t->top_line = t->top_off = 0; t->nlines = 1; t->hscroll = 0; t->want_col = -1;
    if (n) ta_insert(t, 0, s, n);
    t->modified = false;
    ui_invalidate(&t->w);
}

static textarea_t *ta_new(window_t *win, int x, int y, int w, int h)
{
    textarea_t *t = ui_new(win, &ta_class, sizeof(textarea_t), x, y, w, h);
    if (!t) return NULL;
    t->w.flags = WF_TABSTOP | WF_IBEAM | WF_WANT_ENTER | WF_WANT_TAB | WF_WANT_CHARS | WF_CARET | WF_WANT_ARROWS;
    ta_set_text(t, "", 0);
    return t;
}

// ================================================================ editor window

typedef struct {
    window_t *win;
    textarea_t *ta;
    widget_t *status;
    char path[300];
    char find[64];
    bool shown_modified;
} editor_t;

enum { EC_NEW = 1, EC_OPEN, EC_SAVE, EC_SAVE_AS, EC_EXIT, EC_CUT, EC_COPY, EC_PASTE, EC_SELECT_ALL, EC_FIND, EC_FIND_NEXT };

static void ed_title(editor_t *ed)
{
    char t[96];
    snprintf(t, sizeof(t), "%s%s - Editor", ed->ta->modified ? "*" : "", ed->path[0] ? fs_basename(ed->path) : "Untitled");
    wm_set_title(ed->win, t);
    ed->shown_modified = ed->ta->modified;
}

static void ed_status(widget_t *w)
{
    editor_t *ed = w->win->user;
    textarea_t *t = ed->ta;
    int ls = ta_line_start(t, t->cur);
    int line = t->top_line + (t->cur >= t->top_off ? ta_count_nl(t, t->top_off, t->cur) : -ta_count_nl(t, t->cur, t->top_off));
    char s[96];
    snprintf(s, sizeof(s), "Line %d, column %d      %d lines, %d bytes", line + 1, ta_vcol(t, ls, t->cur) + 1, t->nlines, ta_len(t));
    ui_set_text(ed->status, s);
    if (t->modified != ed->shown_modified) ed_title(ed);
}

static bool ed_write(editor_t *ed, const char *path)
{
    // write a temporary file first, so a power cut in the middle never loses the old version
    char tmp[310];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    textarea_t *t = ed->ta;
    bool ok = true;
    if (t->gap0) ok = fwrite(t->buf, 1, t->gap0, f) == (size_t)t->gap0;
    int tail = t->cap - t->gap1;
    if (ok && tail) ok = fwrite(t->buf + t->gap1, 1, tail, f) == (size_t)tail;
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(tmp); return false; }
    unlink(path);
    return rename(tmp, path) == 0;
}

static bool ed_save(editor_t *ed, bool ask_name)
{
    char path[300];
    strlcpy(path, ed->path, sizeof(path));
    if (ask_name || !path[0]) {
        if (!path[0]) strcpy(path, DISK_ROOT "/Documents/Untitled.txt");
        if (!file_dialog(ed->win, true, "Save As", path, sizeof(path))) return false;
    }
    wm_busy(true);
    bool ok = ed_write(ed, path);
    wm_busy(false);
    if (!ok) { msgbox(ed->win, "Editor", "Could not save the file. The drive may be full.", MB_ERROR, MB_OK); return false; }
    strlcpy(ed->path, path, sizeof(ed->path));
    ed->ta->modified = false;
    ed_title(ed);
    return true;
}

// true if it is fine to throw the current text away
static bool ed_can_discard(editor_t *ed)
{
    if (!ed->ta->modified) return true;
    char msg[160];
    snprintf(msg, sizeof(msg), "Save the changes to %s?", ed->path[0] ? fs_basename(ed->path) : "Untitled");
    int r = msgbox(ed->win, "Editor", msg, MB_ASK, MB_YESNOCANCEL);
    if (r == ID_YES) return ed_save(ed, false);
    return r == ID_NO;
}

static bool ed_load(editor_t *ed, const char *path)
{
    int n = 0;
    wm_busy(true);
    char *s = fs_read_all(path, &n);
    wm_busy(false);
    if (!s) return false;
    ta_set_text(ed->ta, s, n);
    free(s);
    strlcpy(ed->path, path, sizeof(ed->path));
    ed_title(ed);
    ed_status(&ed->ta->w);
    return true;
}

static void ed_find(editor_t *ed, bool ask)
{
    if (ask || !ed->find[0]) {
        if (!inputbox(ed->win, "Find", "Find what:", ed->find, sizeof(ed->find)) || !ed->find[0]) return;
    }
    textarea_t *t = ed->ta;
    int n = strlen(ed->find), len = ta_len(t);
    int start = t->cur > t->anchor ? t->cur : t->anchor;
    for (int k = 0; k <= len; k++) {
        int i = (start + k) % (len + 1);
        if (i + n > len) continue;
        int j = 0;
        while (j < n && tolower((unsigned char)ta_ch(t, i + j)) == tolower((unsigned char)ed->find[j])) j++;
        if (j == n) {
            t->anchor = i; t->cur = i + n; t->want_col = -1;
            ta_reveal(t);
            ui_invalidate(&t->w);
            ed_status(&t->w);
            return;
        }
    }
    char msg[120];
    snprintf(msg, sizeof(msg), "\"%s\" was not found.", ed->find);
    msgbox(ed->win, "Find", msg, MB_INFO, MB_OK);
}

static void ed_command(window_t *w, int cmd)
{
    editor_t *ed = w->user;
    event_t e = {.type = EV_KEY, .down = 1, .mods = MOD_CTRL};
    switch (cmd) {
    case EC_NEW:
        if (ed_can_discard(ed)) { ta_set_text(ed->ta, "", 0); ed->path[0] = 0; ed_title(ed); ed_status(&ed->ta->w); }
        break;
    case EC_OPEN: {
        if (!ed_can_discard(ed)) break;
        char path[300];
        strlcpy(path, ed->path, sizeof(path));
        if (file_dialog(w, false, "Open", path, sizeof(path)) && !ed_load(ed, path)) msgbox(w, "Editor", "Could not open the file.", MB_ERROR, MB_OK);
        break;
    }
    case EC_SAVE: ed_save(ed, false); break;
    case EC_SAVE_AS: ed_save(ed, true); break;
    case EC_EXIT: wm_close(w); break;
    case EC_CUT: e.key = K_X; ta_key(&ed->ta->w, &e); break;
    case EC_COPY: e.key = K_C; ta_key(&ed->ta->w, &e); break;
    case EC_PASTE: e.key = K_V; ta_key(&ed->ta->w, &e); break;
    case EC_SELECT_ALL: e.key = K_A; ta_key(&ed->ta->w, &e); break;
    case EC_FIND: ed_find(ed, true); break;
    case EC_FIND_NEXT: ed_find(ed, false); break;
    }
    if (wm_valid(w) && !(w->flags & WIN_DEAD)) ui_focus(&ed->ta->w);
}

static int ed_menu_state(window_t *w, int cmd)
{
    editor_t *ed = w->user;
    if (cmd == EC_CUT || cmd == EC_COPY) return ta_has_sel(ed->ta) ? 0 : MI_DISABLED;
    if (cmd == EC_PASTE) { int n; clip_get(&n); return n ? 0 : MI_DISABLED; }
    return 0;
}

static bool ed_key(window_t *w, const event_t *e)
{
    bool ctrl = e->mods & MOD_CTRL, shift = e->mods & MOD_SHIFT;
    if (e->mods & MOD_ALT) return false;
    if (ctrl) {
        switch (e->key) {
        case K_N: ed_command(w, EC_NEW); return true;
        case K_O: ed_command(w, EC_OPEN); return true;
        case K_S: ed_command(w, shift ? EC_SAVE_AS : EC_SAVE); return true;
        case K_F: ed_command(w, EC_FIND); return true;
        }
    } else if (e->key == K_F3) { ed_command(w, EC_FIND_NEXT); return true; }
    return false;
}

static bool ed_close(window_t *w) { return ed_can_discard(w->user); }

static void ed_resize(window_t *w)
{
    editor_t *ed = w->user;
    int cw = wm_client_w(w), ch = wm_client_h(w);
    ui_set_rect(&ed->ta->w, 2, 2, cw - 4, ch - 22);
    ui_set_rect(ed->status, 6, ch - 16, cw - 12, 14);
    ta_reveal(ed->ta);
}

static void ed_destroy(window_t *w) { free(w->user); }

static const menu_item_t em_file[] = {
    {"&New\tCtrl+N", EC_NEW, NULL, NULL, 0},
    {"&Open...\tCtrl+O", EC_OPEN, NULL, NULL, 0},
    {"&Save\tCtrl+S", EC_SAVE, NULL, NULL, 0},
    {"Save &As...\tCtrl+Shift+S", EC_SAVE_AS, NULL, NULL, 0},
    {"-", 0, NULL, NULL, 0},
    {"E&xit\tAlt+F4", EC_EXIT, NULL, NULL, 0},
    {NULL, 0, NULL, NULL, 0},
};
static const menu_item_t em_edit[] = {
    {"Cu&t\tCtrl+X", EC_CUT, NULL, NULL, 0},
    {"&Copy\tCtrl+C", EC_COPY, NULL, NULL, 0},
    {"&Paste\tCtrl+V", EC_PASTE, NULL, NULL, 0},
    {"-", 0, NULL, NULL, 0},
    {"Select &All\tCtrl+A", EC_SELECT_ALL, NULL, NULL, 0},
    {"-", 0, NULL, NULL, 0},
    {"&Find...\tCtrl+F", EC_FIND, NULL, NULL, 0},
    {"Find &Next\tF3", EC_FIND_NEXT, NULL, NULL, 0},
    {NULL, 0, NULL, NULL, 0},
};
static const menu_item_t em_bar[] = {{"&File", 0, em_file, NULL, 0}, {"&Edit", 0, em_edit, NULL, 0}, {NULL, 0, NULL, NULL, 0}};

void app_editor(const char *arg)
{
    editor_t *ed = calloc(1, sizeof(editor_t));
    window_t *w = wm_create("Editor", 500, 330, WIN_RESIZABLE);
    w->icon = &icon16_text;
    w->user = ed;
    ed->win = w;
    wm_set_menubar(w, em_bar);
    ed->ta = ta_new(w, 2, 2, 496, 308);
    ed->ta->on_change = ed_status;
    ed->status = ui_label(w, 6, 314, 480, 14, "");
    w->on_command = ed_command;
    w->on_menu_state = ed_menu_state;
    w->on_key = ed_key;
    w->on_close = ed_close;
    w->on_resize = ed_resize;
    w->on_destroy = ed_destroy;
    w->min_w = 240; w->min_h = 140;
    ed_title(ed);
    if (arg && arg[0] && !ed_load(ed, arg)) { ed->path[0] = 0; ed_title(ed); }
    ed_status(&ed->ta->w);
    wm_show(w);
    if (arg && arg[0] && !ed->path[0]) msgbox(w, "Editor", "Could not open the file.", MB_ERROR, MB_OK);
}

// Standard dialogs: message box and text prompt.
#include <string.h>
#include <stdlib.h>
#include "wm.h"
#include "audio.h"

static void end_btn(widget_t *w, int code) { wm_end_modal(w->win, w->id); }

// Word-wrap text to max_w pixels; writes '\n' into a copy. Returns the number of lines and the widest line.
static char *wrap_text(const char *text, int max_w, int *lines, int *width)
{
    int n = strlen(text);
    char *out = malloc(n + 1);
    memcpy(out, text, n + 1);
    int line_start = 0, last_space = -1, nl = 1, widest = 0;
    for (int i = 0; i <= n; i++) {
        if (out[i] == ' ') last_space = i;
        if (out[i] == '\n' || out[i] == 0) {
            int w = gfx_text_w(out + line_start, i - line_start, FONT_UI);
            if (w > widest) widest = w;
            if (out[i] == 0) break;
            nl++; line_start = i + 1; last_space = -1;
            continue;
        }
        if (gfx_text_w(out + line_start, i - line_start + 1, FONT_UI) > max_w && last_space > line_start) {
            out[last_space] = '\n';
            i = last_space - 1;
        }
    }
    *lines = nl; *width = widest;
    return out;
}

int msgbox(window_t *owner, const char *title, const char *text, int icon, int buttons)
{
    static const bitmap_t *const icons[] = {&icon32_info, &icon32_warning, &icon32_error, &icon32_question};
    static const struct { const char *label; int id; } defs[][3] = {
        [MB_OK] = {{"OK", ID_OK}},
        [MB_OKCANCEL] = {{"OK", ID_OK}, {"Cancel", ID_CANCEL}},
        [MB_YESNO] = {{"&Yes", ID_YES}, {"&No", ID_NO}},
        [MB_YESNOCANCEL] = {{"&Yes", ID_YES}, {"&No", ID_NO}, {"Cancel", ID_CANCEL}},
    };
    int lines, tw;
    char *wrapped = wrap_text(text, 300, &lines, &tw);
    int nb = buttons == MB_OK ? 1 : buttons == MB_YESNOCANCEL ? 3 : 2;
    int bw = 78, gap = 8;
    int cw = 60 + tw + 16;
    if (cw < nb * (bw + gap) + 24) cw = nb * (bw + gap) + 24;
    if (cw < 200) cw = 200;
    int th = lines * (FONT_UI_H + 2);
    int ch = (th < 36 ? 36 : th) + 24 + 40;
    window_t *d = wm_create(title, cw, ch, WIN_DIALOG);
    d->owner = owner;
    if (owner) { d->r.x = owner->r.x + (owner->r.w - d->r.w) / 2; d->r.y = owner->r.y + (owner->r.h - d->r.h) / 2; }
    if (d->r.x < 0) d->r.x = 0;
    if (d->r.y < 0) d->r.y = 0;
    if (d->r.x + d->r.w > SCREEN_W) d->r.x = SCREEN_W - d->r.w;
    if (d->r.y + d->r.h > DESK_H) d->r.y = DESK_H - d->r.h;
    audio_chime((icon & 3) == MB_ERROR || (icon & 3) == MB_WARN ? CHIME_ERROR : CHIME_INFO);
    ui_icon(d, 14, 14, icons[icon & 3]);
    ui_label(d, 60, 14 + (th < 36 ? (36 - th) / 2 : 0), tw + 4, th, wrapped);
    free(wrapped);
    int bx = (cw - nb * bw - (nb - 1) * gap) / 2, by = ch - 34;
    for (int i = 0; i < nb; i++) {
        widget_t *b = ui_button(d, bx + i * (bw + gap), by, bw, 24, defs[buttons][i].label, end_btn);
        b->id = defs[buttons][i].id;
        if (i == 0) { d->def_btn = b; d->focus = b; }
        if (b->id == ID_CANCEL || (buttons == MB_YESNO && b->id == ID_NO) || buttons == MB_OK) d->cancel_btn = b;
    }
    int r = wm_run_modal(d);
    if (buttons == MB_YESNO && r == ID_CANCEL) r = ID_NO;
    return r;
}

typedef struct { widget_t *field; char *buf; int size; } input_ctx_t;

static void input_btn(widget_t *w, int code)
{
    input_ctx_t *c = w->win->user;
    if (w->id == ID_OK) strlcpy(c->buf, ui_textfield_get(c->field), c->size);
    wm_end_modal(w->win, w->id);
}

static bool s_input_mask;
bool inputbox_password(window_t *owner, const char *title, const char *prompt, char *buf, int buf_size)
{
    s_input_mask = true;
    bool r = inputbox(owner, title, prompt, buf, buf_size);
    s_input_mask = false;
    return r;
}

bool inputbox(window_t *owner, const char *title, const char *prompt, char *buf, int buf_size)
{
    window_t *d = wm_create(title, 300, 104, WIN_DIALOG);
    d->owner = owner;
    input_ctx_t ctx = {NULL, buf, buf_size};
    d->user = &ctx;
    ui_label(d, 12, 12, 276, 14, prompt);
    ctx.field = ui_textfield(d, 12, 32, 276, buf, buf_size - 1);
    if (s_input_mask) ui_textfield_mask(ctx.field, true);
    widget_t *ok = ui_button(d, 124, 68, 78, 24, "OK", input_btn);
    ok->id = ID_OK;
    widget_t *ca = ui_button(d, 210, 68, 78, 24, "Cancel", input_btn);
    ca->id = ID_CANCEL;
    d->def_btn = ok; d->cancel_btn = ca;
    d->focus = ctx.field;
    ui_textfield_select_all(ctx.field);
    return wm_run_modal(d) == ID_OK;
}

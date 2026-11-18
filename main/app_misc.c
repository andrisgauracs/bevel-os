// Settings (saved to the drive), the picture viewer (BMP with dithering) and the terminal.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include "wm.h"
#include "apps.h"
#include "os.h"
#include "comp.h"
#include "display.h"
#include "audio.h"
#include "fsutil.h"
#include "layout.h"
#include "net.h"
#include "esp_heap_caps.h"

// ================================================================ settings

#define SETTINGS_FILE DISK_ROOT "/System/settings.ini"

static const struct { const char *name; uint8_t color; } s_colors[] = {
    {"Teal", RGB222(0, 2, 2)}, {"Navy", RGB222(0, 0, 2)}, {"Slate", RGB222(1, 1, 2)}, {"Forest", RGB222(0, 1, 0)},
    {"Plum", RGB222(1, 0, 1)}, {"Maroon", RGB222(1, 0, 0)}, {"Olive", RGB222(1, 1, 0)}, {"Storm", RGB222(0, 1, 1)},
    {"Grey", RGB222(1, 1, 1)}, {"Black", RGB222(0, 0, 0)}, {"Sky", RGB222(1, 2, 3)}, {"Sand", RGB222(2, 2, 1)},
};
#define NCOLORS ((int)(sizeof(s_colors) / sizeof(s_colors[0])))

void settings_load(void)
{
    int n;
    char *s = fs_read_all(SETTINGS_FILE, &n);
    clock_set_zone(0);
    if (!s) return;
    for (char *line = strtok(s, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "desktop") && v >= 0 && v < 64) shell_desktop_color = v;
        else if (!strcmp(line, "mouse_speed") && v >= 1 && v <= 10) input_mouse_speed = v;
        else if (!strcmp(line, "repeat_delay") && v >= 100 && v <= 1000) input_repeat_delay = v;
        else if (!strcmp(line, "repeat_rate") && v >= 2 && v <= 50) input_repeat_rate = v;
        // keys for controls this board does not have are skipped, so a settings file written on the
        // other board still loads
#if DISP_HAS_BACKLIGHT
        else if (!strcmp(line, "brightness") && v >= 10 && v <= 100) disp_set_brightness(v);
#endif
#if BOARD_HAS_AUDIO
        else if (!strcmp(line, "volume") && v >= 0 && v <= 100) audio_volume = v;
        else if (!strcmp(line, "sounds")) audio_system_sounds = v != 0;
        else if (!strcmp(line, "audio_output")) audio_set_output(v);
#endif
        else if (!strcmp(line, "zone")) {
            for (int i = 0; i < clock_zone_count(); i++) if (!strcmp(eq + 1, clock_zone_name(i))) clock_set_zone(i);
        }
        else if (!strcmp(line, "layout")) {
            for (int i = 0; i < layout_count; i++)
                if (!strcmp(eq + 1, layout_name[i])) { input_layout_set(i); serial_printf("layout set %d from settings\n", i); }
        }
        else if (!strcmp(line, "remote")) remote_set(v != 0);
    }
    free(s);
}

bool settings_save(void)
{
    // temp file + rename: an unplugged cable never leaves half a settings file
    FILE *f = fopen(SETTINGS_FILE ".tmp", "wb");
    if (!f) return false;
    // a board only ever writes the settings its own controls have, so a card or a drive moved
    // between the two boards carries no keys its new board would have to invent a control for
    fprintf(f, "desktop=%d\nmouse_speed=%d\nrepeat_delay=%d\nrepeat_rate=%d\n"
#if DISP_HAS_BACKLIGHT
               "brightness=%d\n"
#endif
#if BOARD_HAS_AUDIO
               "volume=%d\nsounds=%d\naudio_output=%d\n"
#endif
               "zone=%s\nlayout=%s\nremote=%d\n",
            shell_desktop_color, input_mouse_speed, input_repeat_delay, input_repeat_rate,
#if DISP_HAS_BACKLIGHT
            disp_get_brightness(),
#endif
#if BOARD_HAS_AUDIO
            audio_volume, audio_system_sounds, audio_output(),
#endif
            clock_zone_name(clock_zone()), layout_name[input_layout_get()], remote_on());
    if (fclose(f) != 0) return false;
    unlink(SETTINGS_FILE);
    return rename(SETTINGS_FILE ".tmp", SETTINGS_FILE) == 0;
}

static window_t *s_set;
static struct {
    widget_t *colors, *speed, *delay, *rate, *bright, *volume, *output, *sounds, *zones, *layouts, *remote, *remote_url;
    uint8_t old_color; int old_speed, old_delay, old_rate, old_bright, old_volume, old_output, old_sounds, old_zone, old_layout;
    bool old_remote;
} s_sd;

static void colors_get(widget_t *lv, int i, lv_item_t *it) { it->text = s_colors[i].name; }

static void zones_get(widget_t *lv, int i, lv_item_t *it) { it->text = clock_zone_name(i); }

static void layouts_get(widget_t *lv, int i, lv_item_t *it) { it->text = layout_name[i]; }

#if BOARD_HAS_AUDIO
static void outputs_get(widget_t *lv, int i, lv_item_t *it) { it->text = audio_output_name(i); }

// The taskbar's volume icon changed something and has saved it: an open Settings window follows, and
// Cancel no longer takes it back.
void settings_sound_changed(void)
{
    if (!s_set) return;
    s_sd.old_volume = audio_volume;
    s_sd.old_output = audio_output();
    ui_slider_set(s_sd.volume, audio_volume / 5);
    ui_dropdown_select(s_sd.output, audio_output());
}
#endif

// what the Remote access group says under its switch
static void remote_text(void)
{
    char t[64];
    net_status_t st;
    net_status(&st);
    const char *ip = st.eth_ip[0] ? st.eth_ip : st.state == NET_CONNECTED ? st.ip : "";
    if (!remote_on()) strcpy(t, "Off");
    else if (ip[0]) snprintf(t, sizeof(t), "Open http://%s/", ip);
    else strcpy(t, "On, once the network is connected");
    ui_set_text(s_sd.remote_url, t);
}

static void set_apply(widget_t *w, int code)
{
    int i = ui_listview_sel(s_sd.colors);
    if (i >= 0 && s_colors[i].color != shell_desktop_color) { shell_desktop_color = s_colors[i].color; comp_invalidate_all(); }
    input_mouse_speed = ui_slider_get(s_sd.speed);
    input_repeat_delay = 100 + ui_slider_get(s_sd.delay) * 100;
    input_repeat_rate = ui_slider_get(s_sd.rate) * 5;
#if DISP_HAS_BACKLIGHT
    disp_set_brightness(ui_slider_get(s_sd.bright) * 10);
#endif
#if BOARD_HAS_AUDIO
    int vol = ui_slider_get(s_sd.volume) * 5;
    audio_system_sounds = ui_checked(s_sd.sounds);
    if (vol != audio_volume) { audio_volume = vol; if (w == s_sd.volume) audio_chime(CHIME_INFO); }
    int out = ui_dropdown_sel(s_sd.output);
    if (out >= 0 && out != audio_output()) { audio_set_output(out); audio_chime(CHIME_INFO); }
#endif
    int z = ui_dropdown_sel(s_sd.zones);
    if (z >= 0 && z != clock_zone()) clock_set_zone(z);
    int l = ui_dropdown_sel(s_sd.layouts);
    if (l >= 0 && l != input_layout_get()) input_layout_set(l);  // typing changes as you pick
    if (ui_checked(s_sd.remote) != remote_on()) { remote_set(ui_checked(s_sd.remote)); remote_text(); }
    // preview swatch
    for (widget_t *x = s_set->widgets; x; x = x->next) if (!strcmp(x->cls->name, "canvas")) ui_invalidate(x);
}

static void swatch_paint(widget_t *w, gfx_t *g)
{
    gfx_fill(g, 0, 0, w->r.w, w->r.h, shell_desktop_color);
    ui_draw_sunken(g, 0, 0, w->r.w, w->r.h);
    gfx_bitmap(g, (w->r.w - 32) / 2, 8, &icon32_folder);
    gfx_text(g, (w->r.w - gfx_text_w("Folder", -1, FONT_UI)) / 2, 44, "Folder", -1, FONT_UI, C_WHITE);
}
static const canvas_ops_t swatch_ops = {swatch_paint, NULL, NULL};

static void set_ok(widget_t *b, int code)
{
    set_apply(b, 0);
    if (!settings_save()) msgbox(b->win, "Settings", "The settings are in use but could not be saved to the drive.", MB_WARN, MB_OK);
    wm_destroy(s_set);
}

static void set_cancel(widget_t *b, int code)
{
    shell_desktop_color = s_sd.old_color;
    input_mouse_speed = s_sd.old_speed;
    input_repeat_delay = s_sd.old_delay;
    input_repeat_rate = s_sd.old_rate;
#if DISP_HAS_BACKLIGHT
    disp_set_brightness(s_sd.old_bright);
#endif
#if BOARD_HAS_AUDIO
    audio_volume = s_sd.old_volume;
    audio_system_sounds = s_sd.old_sounds;
    audio_set_output(s_sd.old_output);
#endif
    clock_set_zone(s_sd.old_zone);
    input_layout_set(s_sd.old_layout);
    if (remote_on() != s_sd.old_remote) remote_set(s_sd.old_remote);
    comp_invalidate_all();
    wm_destroy(s_set);
}

static bool set_close(window_t *w) { set_cancel(NULL, 0); return false; }
static void set_destroy(window_t *w) { s_set = NULL; }

void app_settings(const char *arg)
{
    if (s_set) { wm_activate(s_set); return; }
    // the groups end at x = 630: on the S3's 640-pixel screen the client area is 632 wide, so the
    // frame fits the screen exactly instead of hanging 8 pixels off its right edge
    int sw = SCREEN_W - 2 * BORDER_W < 640 ? SCREEN_W - 2 * BORDER_W : 640;
    window_t *w = wm_create("Settings", sw, 334, WIN_DIALOG);
    w->icon = &icon16_settings;
    s_set = w;
    s_sd.old_color = shell_desktop_color; s_sd.old_speed = input_mouse_speed;
    s_sd.old_delay = input_repeat_delay; s_sd.old_rate = input_repeat_rate;
#if DISP_HAS_BACKLIGHT
    s_sd.old_bright = disp_get_brightness();
#endif
#if BOARD_HAS_AUDIO
    s_sd.old_volume = audio_volume; s_sd.old_sounds = audio_system_sounds; s_sd.old_output = audio_output();
#endif
    s_sd.old_zone = clock_zone();
    s_sd.old_layout = input_layout_get();
    s_sd.old_remote = remote_on();

    ui_groupbox(w, 10, 8, 380, 124, "Desktop");
    ui_label(w, 22, 28, 100, 14, "&Colour:");
    s_sd.colors = ui_listview(w, 22, 44, 180, 78, colors_get);
    ui_listview_set_count(s_sd.colors, NCOLORS);
    for (int i = 0; i < NCOLORS; i++) if (s_colors[i].color == shell_desktop_color) ui_listview_select(s_sd.colors, i);
    s_sd.colors->on_action = set_apply;
    ui_canvas(w, 230, 44, 140, 78, &swatch_ops, false);

    ui_groupbox(w, 10, 138, 380, 46, "Mouse");
    ui_label(w, 22, 160, 100, 14, "Pointer &speed:");
    ui_label(w, 122, 160, 30, 14, "Slow");
    s_sd.speed = ui_slider(w, 152, 154, 190, 1, 10, input_mouse_speed);
    s_sd.speed->on_action = set_apply;
    ui_label(w, 350, 160, 30, 14, "Fast");

    ui_groupbox(w, 10, 190, 380, 106, "Keyboard");
    // the layout is what the keys print: picked from the list, and typing changes straight away
    ui_label(w, 22, 212, 100, 14, "Keyboard &layout:");
    s_sd.layouts = ui_dropdown(w, 122, 208, 250, layouts_get);
    ui_dropdown_set_count(s_sd.layouts, layout_count);
    ui_dropdown_select(s_sd.layouts, input_layout_get());
    s_sd.layouts->on_action = set_apply;
    ui_label(w, 22, 246, 100, 14, "Repeat &delay:");
    ui_label(w, 122, 246, 30, 14, "Short");
    s_sd.delay = ui_slider(w, 152, 240, 190, 1, 8, (input_repeat_delay - 100) / 100);
    s_sd.delay->on_action = set_apply;
    ui_label(w, 350, 246, 30, 14, "Long");
    ui_label(w, 22, 272, 100, 14, "Repeat &rate:");
    ui_label(w, 122, 272, 30, 14, "Slow");
    s_sd.rate = ui_slider(w, 152, 266, 190, 1, 8, input_repeat_rate / 5);
    s_sd.rate->on_action = set_apply;
    ui_label(w, 350, 272, 30, 14, "Fast");

    // The right-hand column holds only the groups this board has hardware for, stacked from the top
    // so a board without a backlight or without sound has no hole where those groups would be. It
    // never reaches below the keyboard group, so OK and Cancel go under the left column's end.
    int gy = 8;
#if DISP_HAS_BACKLIGHT
    ui_groupbox(w, 400, gy, 230, 46, "Screen");
    ui_label(w, 412, gy + 22, 70, 14, "&Brightness:");
    s_sd.bright = ui_slider(w, 484, gy + 16, 136, 1, 10, (disp_get_brightness() + 5) / 10);
    s_sd.bright->on_action = set_apply;
    gy += 52;
#endif
#if BOARD_HAS_AUDIO
    ui_groupbox(w, 400, gy, 230, 96, "Sound");
    ui_label(w, 412, gy + 22, 70, 14, "&Volume:");
    s_sd.volume = ui_slider(w, 484, gy + 16, 136, 0, 20, audio_volume / 5);
    s_sd.volume->on_action = set_apply;
    ui_label(w, 412, gy + 46, 70, 14, "&Output:");
    s_sd.output = ui_dropdown(w, 484, gy + 42, 136, outputs_get);
    ui_dropdown_set_count(s_sd.output, AUDIO_OUT_COUNT);
    ui_dropdown_select(s_sd.output, audio_output());
    s_sd.output->on_action = set_apply;
    s_sd.sounds = ui_checkbox(w, 412, gy + 70, 200, "S&ystem sounds", audio_system_sounds);
    s_sd.sounds->on_action = set_apply;
    gy += 102;
#endif
    ui_groupbox(w, 400, gy, 230, 68, "Clock");
    ui_label(w, 412, gy + 20, 100, 14, "Time &zone:");
    s_sd.zones = ui_dropdown(w, 412, gy + 36, 206, zones_get);
    ui_dropdown_set_count(s_sd.zones, clock_zone_count());
    ui_dropdown_select(s_sd.zones, clock_zone());
    s_sd.zones->on_action = set_apply;
    gy += 74;

    // the screen in a web browser elsewhere: no password, so it is off until switched on here
    ui_groupbox(w, 400, gy, 230, 64, "Remote access");
    s_sd.remote = ui_checkbox(w, 412, gy + 18, 210, "&Web page with this screen", remote_on());
    s_sd.remote->on_action = set_apply;
    s_sd.remote_url = ui_label(w, 430, gy + 40, 192, 14, "");
    remote_text();

    widget_t *ok = ui_button(w, 468, 302, 78, 24, "OK", set_ok);
    widget_t *ca = ui_button(w, 552, 302, 78, 24, "Cancel", set_cancel);
    w->def_btn = ok; w->cancel_btn = ca;
    w->on_close = set_close;
    w->on_destroy = set_destroy;
    w->focus = s_sd.colors;
    wm_show(w);
}

// ================================================================ picture viewer

typedef struct {
    window_t *win;
    widget_t *canvas;
    uint8_t *px;
    int w, h, zoom, sx, sy;
    bool drag;
    int dx, dy;
} viewer_t;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

// Load a BMP and convert it to RGB222 with Floyd-Steinberg dithering. Returns NULL and sets *err.
uint8_t *bmp_load(const char *path, int *out_w, int *out_h, const char **err)
{
    FILE *f = fopen(path, "rb");
    *err = "The file could not be opened.";
    if (!f) return NULL;
    uint8_t hdr[54];
    uint8_t *out = NULL, *row = NULL;
    int16_t *e0 = NULL, *e1 = NULL;
    uint8_t pal[256][4];
    *err = "This is not a BMP picture.";
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') goto done;
    uint32_t data_off = rd32(hdr + 10), hsize = rd32(hdr + 14);
    int w = (int32_t)rd32(hdr + 18), h = (int32_t)rd32(hdr + 22);
    int bpp = rd16(hdr + 28);
    uint32_t comp = rd32(hdr + 30);
    bool flip = h > 0;
    if (h < 0) h = -h;
    *err = "This kind of BMP is not supported (only uncompressed 1, 4, 8, 24 and 32 bit).";
    if ((comp != 0 && !(comp == 3 && bpp == 32)) || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)) goto done;
    *err = "The picture is too large (the limit is 2048 x 2048).";
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) goto done;
    if (bpp <= 8) {
        int ncol = rd32(hdr + 46);
        if (ncol <= 0 || ncol > 256) ncol = 1 << bpp;
        fseek(f, 14 + hsize, SEEK_SET);
        memset(pal, 0, sizeof(pal));
        if (fread(pal, 4, ncol, f) != (size_t)ncol) { *err = "The picture file is damaged."; goto done; }
    }
    int stride = ((w * bpp + 31) / 32) * 4;
    *err = "Not enough memory for this picture.";
    out = heap_caps_malloc(w * h, MALLOC_CAP_SPIRAM);
    row = heap_caps_malloc(stride, MALLOC_CAP_SPIRAM);
    e0 = heap_caps_calloc((w + 2) * 3, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    e1 = heap_caps_calloc((w + 2) * 3, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!out || !row || !e0 || !e1) { free(out); out = NULL; goto done; }
    for (int y = 0; y < h; y++) {
        // rows are stored bottom-up; read them in display order so the dither runs top-down
        int file_row = flip ? h - 1 - y : y;
        fseek(f, data_off + (long)file_row * stride, SEEK_SET);
        if (fread(row, 1, stride, f) != (size_t)stride) { *err = "The picture file is damaged."; free(out); out = NULL; goto done; }
        memset(e1, 0, (w + 2) * 3 * sizeof(int16_t));
        uint8_t *d = out + y * w;
        for (int x = 0; x < w; x++) {
            int c[3]; // r, g, b
            if (bpp >= 24) { const uint8_t *p = row + x * (bpp / 8); c[0] = p[2]; c[1] = p[1]; c[2] = p[0]; }
            else {
                int idx = bpp == 8 ? row[x] : bpp == 4 ? (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15 : (row[x >> 3] >> (7 - (x & 7))) & 1;
                c[0] = pal[idx][2]; c[1] = pal[idx][1]; c[2] = pal[idx][0];
            }
            uint8_t pix = 0;
            for (int k = 0; k < 3; k++) {
                int v = c[k] + e0[(x + 1) * 3 + k] / 16;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                int q = (v + 42) / 85;          // nearest of 0, 85, 170, 255
                int err_v = v - q * 85;
                pix |= q << (k * 2);
                e0[(x + 2) * 3 + k] += err_v * 7;
                e1[x * 3 + k] += err_v * 3;
                e1[(x + 1) * 3 + k] += err_v * 5;
                e1[(x + 2) * 3 + k] += err_v;
            }
            d[x] = pix;
        }
        int16_t *t = e0; e0 = e1; e1 = t;
    }
    *out_w = w; *out_h = h;
    *err = NULL;
done:
    free(row); free(e0); free(e1);
    fclose(f);
    return out;
}

static void view_clamp(viewer_t *v)
{
    int vw = v->canvas->r.w / v->zoom, vh = v->canvas->r.h / v->zoom;
    if (v->sx > v->w - vw) v->sx = v->w - vw;
    if (v->sy > v->h - vh) v->sy = v->h - vh;
    if (v->sx < 0) v->sx = 0;
    if (v->sy < 0) v->sy = 0;
}

static void view_paint(widget_t *w, gfx_t *g)
{
    viewer_t *v = w->win->user;
    int z = v->zoom;
    int iw = v->w * z, ih = v->h * z;
    int ox = iw < w->r.w ? (w->r.w - iw) / 2 : -v->sx * z;
    int oy = ih < w->r.h ? (w->r.h - ih) / 2 : -v->sy * z;
    if (iw < w->r.w || ih < w->r.h) gfx_fill(g, 0, 0, w->r.w, w->r.h, C_DKGREY);
    if (z == 1) { gfx_blit(g, ox, oy, v->w, v->h, v->px, v->w); return; }
    // zoomed: expand one source row at a time, only the rows inside the clip
    uint8_t line[SCREEN_W];
    int y_first = (g->cy0 - g->oy - oy) / z, y_last = (g->cy1 - 1 - g->oy - oy) / z;
    int x_first = (g->cx0 - g->ox - ox) / z, x_last = (g->cx1 - 1 - g->ox - ox) / z;
    if (y_first < 0) y_first = 0;
    if (x_first < 0) x_first = 0;
    if (y_last >= v->h) y_last = v->h - 1;
    if (x_last >= v->w) x_last = v->w - 1;
    if (x_last < x_first) return;
    int n = (x_last - x_first + 1) * z;
    if (n > SCREEN_W) n = SCREEN_W;
    for (int y = y_first; y <= y_last; y++) {
        const uint8_t *s = v->px + y * v->w + x_first;
        for (int i = 0; i < n; i++) line[i] = s[i / z];
        for (int k = 0; k < z; k++) gfx_blit(g, ox + x_first * z, oy + y * z + k, n, 1, line, n);
    }
}

static void view_zoom(viewer_t *v, int z)
{
    if (z < 1) z = 1;
    if (z > 4) z = 4;
    if (z == v->zoom) return;
    // keep the centre in place
    int cx = v->sx + v->canvas->r.w / v->zoom / 2, cy = v->sy + v->canvas->r.h / v->zoom / 2;
    v->zoom = z;
    v->sx = cx - v->canvas->r.w / z / 2;
    v->sy = cy - v->canvas->r.h / z / 2;
    view_clamp(v);
    ui_invalidate(v->canvas);
}

static bool view_key(widget_t *w, const event_t *e)
{
    viewer_t *v = w->win->user;
    if (!e->down) return false;
    int step = (e->mods & MOD_CTRL) ? 1 : 24;
    switch (e->key) {
    case K_LEFT: v->sx -= step; break;
    case K_RIGHT: v->sx += step; break;
    case K_UP: v->sy -= step; break;
    case K_DOWN: v->sy += step; break;
    case K_PGUP: v->sy -= w->r.h / v->zoom; break;
    case K_PGDN: v->sy += w->r.h / v->zoom; break;
    case K_HOME: v->sx = v->sy = 0; break;
    case K_END: v->sx = v->w; v->sy = v->h; break;
    case K_ESC: wm_close(w->win); return true;
    default:
        if (e->ch == '+' || e->ch == '=') { view_zoom(v, v->zoom + 1); return true; }
        if (e->ch == '-') { view_zoom(v, v->zoom - 1); return true; }
        return false;
    }
    view_clamp(v);
    ui_invalidate(w);
    return true;
}

static void view_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    viewer_t *v = w->win->user;
    if (type == EV_MOUSE_DOWN && btn == BTN_LEFT) {
        if (clicks == 2) { view_zoom(v, v->zoom >= 4 ? 1 : v->zoom + 1); return; }
        v->drag = true; v->dx = x; v->dy = y;
    } else if (type == EV_MOUSE_MOVE && v->drag) {
        int mx = (v->dx - x) / v->zoom, my = (v->dy - y) / v->zoom;
        if (mx || my) { v->sx += mx; v->sy += my; v->dx -= mx * v->zoom; v->dy -= my * v->zoom; view_clamp(v); ui_invalidate(w); }
    } else if (type == EV_MOUSE_UP) v->drag = false;
    else if (type == EV_WHEEL) { v->sy -= btn * 24; view_clamp(v); ui_invalidate(w); }
}
static const canvas_ops_t view_ops = {view_paint, view_key, view_mouse};

static void view_resize(window_t *w)
{
    viewer_t *v = w->user;
    ui_set_rect(v->canvas, 0, 0, wm_client_w(w), wm_client_h(w));
    view_clamp(v);
}
static void view_destroy(window_t *w) { viewer_t *v = w->user; free(v->px); free(v); }

void app_viewer(const char *arg)
{
    char path[300] = "";
    if (arg && arg[0]) strlcpy(path, arg, sizeof(path));
    else {
        strcpy(path, DISK_ROOT "/Pictures");
        if (!file_dialog(NULL, false, "Open Picture", path, sizeof(path))) return;
    }
    const char *err;
    int iw, ih;
    wm_busy(true);
    uint8_t *px = bmp_load(path, &iw, &ih, &err);
    wm_busy(false);
    if (!px) { msgbox(NULL, "Picture Viewer", err, MB_ERROR, MB_OK); return; }
    viewer_t *v = calloc(1, sizeof(viewer_t));
    v->px = px; v->w = iw; v->h = ih; v->zoom = 1;
    // the window fits the picture; a picture larger than the desktop gets the whole desktop (the
    // window opens maximised, and restores to the desktop size less its frame)
    int cw = iw < 160 ? 160 : iw, ch = ih < 100 ? 100 : ih;
    char title[96];
    snprintf(title, sizeof(title), "%s (%d x %d) - Picture Viewer", fs_basename(path), iw, ih);
    window_t *w = wm_create(title, cw, ch, WIN_RESIZABLE);
    w->icon = &icon16_image;
    w->user = v;
    v->win = w;
    int fw = w->r.w - cw, fh = w->r.h - ch;         // the frame around the picture
    bool too_big = w->r.w > SCREEN_W || w->r.h > DESK_H;
    if (too_big) {
        cw = SCREEN_W - fw;
        ch = DESK_H - fh;
        wm_resize(w, SCREEN_W, DESK_H);
        wm_move(w, 0, 0);
    }
    v->canvas = ui_canvas(w, 0, 0, cw, ch, &view_ops, true);
    w->on_resize = view_resize;
    w->on_destroy = view_destroy;
    wm_show(w);
    if (too_big) wm_maximize(w, true);
}

// ================================================================ terminal

#define T_COLS 72
#define T_ROWS 22
#define T_HIST 200
typedef struct {
    window_t *win;
    widget_t *canvas;
    char (*lines)[T_COLS + 1];
    int head, count;        // ring of T_HIST lines
    int scroll;             // lines scrolled back
    char input[160];
    int ilen;
    char cwd[256];
    char last[160];
} term_t;

static void term_newline(term_t *t)
{
    t->head = (t->head + 1) % T_HIST;
    t->lines[t->head][0] = 0;
    if (t->count < T_HIST) t->count++;
}

static void term_puts(term_t *t, const char *s)
{
    char *line = t->lines[t->head];
    int n = strlen(line);
    for (; *s; s++) {
        if (*s == '\r') continue;
        if (*s == '\n') { term_newline(t); line = t->lines[t->head]; n = 0; continue; }
        if (n >= T_COLS) { term_newline(t); line = t->lines[t->head]; n = 0; }
        line[n++] = *s == '\t' ? ' ' : *s;
        line[n] = 0;
    }
}

static void term_printf(term_t *t, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(t, buf);
}

static void term_path(term_t *t, const char *arg, char *out, int n)
{
    char tmp[300];
    if (!arg || !arg[0]) { strlcpy(out, t->cwd, n); return; }
    if (arg[0] == '/') snprintf(tmp, sizeof(tmp), "%s%s", DISK_ROOT, arg);
    else fs_join(tmp, sizeof(tmp), t->cwd, arg);
    // resolve . and ..
    char *parts[32];
    int np = 0;
    for (char *p = strtok(tmp + strlen(DISK_ROOT), "/"); p && np < 32; p = strtok(NULL, "/")) {
        if (!strcmp(p, ".")) continue;
        if (!strcmp(p, "..")) { if (np) np--; continue; }
        parts[np++] = p;
    }
    strlcpy(out, DISK_ROOT, n);
    for (int i = 0; i < np; i++) { strlcat(out, "/", n); strlcat(out, parts[i], n); }
}

static const char *term_show(const char *path) { return path[strlen(DISK_ROOT)] ? path + strlen(DISK_ROOT) : "/"; }

// Names are taken as typed: "sd" is not "SD". FAT would find either, and for the card's mount point
// that means a different, empty folder on the drive, so every lookup checks the exact spelling.
static bool term_dir(const char *p) { return fs_is_dir(p) && fs_exists_exact(p); }
static bool term_exists(const char *p) { return fs_exists_exact(p); }
static bool term_parent_ok(const char *p)
{
    char d[300];
    strlcpy(d, p, sizeof(d));
    char *s = strrchr(d, '/');
    if (s && s > d) *s = 0;
    return term_dir(d);
}

static void term_exec(term_t *t, char *line)
{
    char *argv[4] = {0};
    int argc = 0;
    for (char *p = line; *p && argc < 4;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') { argv[argc++] = ++p; while (*p && *p != '"') p++; }
        else { argv[argc++] = p; if (argc == 4) break; while (*p && *p != ' ') p++; }
        if (*p) *p++ = 0;
    }
    if (!argc) return;
    const char *c = argv[0];
    char a[300], b[300];
    if (!strcmp(c, "help")) {
        term_puts(t, "ls [dir]    list a folder        cd <dir>   change folder\n"
                     "cat <file>  show a text file     pwd        show the current folder\n"
                     "mkdir <d>   make a folder        rm <name>  delete a file or folder\n"
                     "cp <a> <b>  copy                 mv <a> <b> move or rename\n"
                     "edit <f>    open in the editor   run <app>  start a program\n"
                     "open <f>    open with its app    echo <txt> print text\n"
                     "mem         memory in use        df         drive space\n"
                     "ver         version              uptime     time since power-on\n"
                     "clear       clear the screen     exit       close the terminal\n"
                     "eject       unmount the USB stick\n");
    } else if (!strcmp(c, "ls") || !strcmp(c, "dir")) {
        term_path(t, argv[1], a, sizeof(a));
        fs_entry_t *list;
        if (!term_dir(a) && term_exists(a)) {          // a file: its own line
            struct stat st;
            char sz[24] = "?";
            if (stat(a, &st) == 0) os_fmt_size(sz, sizeof(sz), st.st_size);
            term_printf(t, "%12s  %s\n", sz, fs_basename(a));
            return;
        }
        int n = term_dir(a) ? fs_list(a, &list) : -1;
        if (n < 0) { term_printf(t, "ls: no such folder: %s\n", argv[1] ? argv[1] : ""); return; }
        for (int i = 0; i < n; i++) {
            char sz[24] = "<folder>";
            if (!list[i].is_dir) os_fmt_size(sz, sizeof(sz), list[i].size);
            term_printf(t, "%12s  %s\n", sz, list[i].name);
        }
        term_printf(t, "%d item%s\n", n, n == 1 ? "" : "s");
        free(list);
    } else if (!strcmp(c, "cd")) {
        term_path(t, argv[1] ? argv[1] : "/", a, sizeof(a));
        if (term_dir(a)) strlcpy(t->cwd, a, sizeof(t->cwd)); else term_printf(t, "cd: no such folder: %s\n", argv[1]);
    } else if (!strcmp(c, "pwd")) {
        term_printf(t, "%s\n", term_show(t->cwd));
    } else if (!strcmp(c, "cat") || !strcmp(c, "type")) {
        if (!argv[1]) { term_puts(t, "cat: which file?\n"); return; }
        term_path(t, argv[1], a, sizeof(a));
        int n;
        char *s = fs_is_dir(a) || !term_exists(a) ? NULL : fs_read_all(a, &n);
        if (!s) { term_printf(t, "cat: cannot read %s\n", argv[1]); return; }
        if (n > 16000) { s[16000] = 0; }
        for (int i = 0; s[i]; i++) if ((unsigned char)s[i] < 32 && s[i] != '\n' && s[i] != '\t' && s[i] != '\r') s[i] = '.';
        term_puts(t, s);
        if (n && s[n > 16000 ? 15999 : n - 1] != '\n') term_puts(t, "\n");
        free(s);
    } else if (!strcmp(c, "mkdir")) {
        if (!argv[1]) { term_puts(t, "mkdir: which name?\n"); return; }
        term_path(t, argv[1], a, sizeof(a));
        if (!term_parent_ok(a)) term_printf(t, "mkdir: no such folder: %s\n", argv[1]);
        else if (mkdir(a, 0777) != 0) term_printf(t, "mkdir: cannot create %s\n", argv[1]);
    } else if (!strcmp(c, "rm") || !strcmp(c, "del") || !strcmp(c, "rmdir")) {
        if (!argv[1]) { term_puts(t, "rm: which name?\n"); return; }
        term_path(t, argv[1], a, sizeof(a));
        if (!strcmp(a, DISK_ROOT) || !term_exists(a)) term_printf(t, "rm: no such file: %s\n", argv[1]);
        else if (!fs_delete(a)) term_printf(t, "rm: cannot delete %s\n", argv[1]);
    } else if (!strcmp(c, "cp") || !strcmp(c, "mv")) {
        if (!argv[1] || !argv[2]) { term_printf(t, "%s: needs a source and a destination\n", c); return; }
        term_path(t, argv[1], a, sizeof(a));
        term_path(t, argv[2], b, sizeof(b));
        if (term_dir(b)) { char tmp[300]; fs_join(tmp, sizeof(tmp), b, fs_basename(a)); strlcpy(b, tmp, sizeof(b)); }
        if (!term_exists(a)) term_printf(t, "%s: no such file: %s\n", c, argv[1]);
        else if (!term_parent_ok(b)) term_printf(t, "%s: no such folder: %s\n", c, argv[2]);
        else if (c[0] == 'c' ? !fs_copy(a, b) : rename(a, b) != 0) term_printf(t, "%s: failed\n", c);
    } else if (!strcmp(c, "edit")) {
        term_path(t, argv[1], a, sizeof(a));
        if (argv[1] && !term_parent_ok(a)) { term_printf(t, "edit: no such folder: %s\n", argv[1]); return; }
        app_editor(argv[1] ? a : NULL);
    } else if (!strcmp(c, "open") || !strcmp(c, "run")) {
        if (!argv[1]) { term_printf(t, "%s: which file?\n", c); return; }
        term_path(t, argv[1], a, sizeof(a));
        if (!term_exists(a) && c[0] == 'r') { snprintf(b, sizeof(b), DISK_ROOT "/Apps/%s", argv[1]); strlcpy(a, b, sizeof(a)); if (!term_exists(a)) strlcat(a, ".elf", sizeof(a)); }
        if (!term_exists(a)) term_printf(t, "%s: no such file: %s\n", c, argv[1]);
        else if (fs_is_dir(a)) app_files(a);
        else os_open_file(t->win, a);
    } else if (!strcmp(c, "echo")) {
        for (int i = 1; i < argc; i++) term_printf(t, "%s%s", argv[i], i < argc - 1 ? " " : "");
        term_puts(t, "\n");
    } else if (!strcmp(c, "mem")) {
        term_printf(t, "internal RAM: %u KB free of %u KB (largest block %u KB)\n",
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024),
                    (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        term_printf(t, "PSRAM:        %u KB free of %u KB\n", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                    (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    } else if (!strcmp(c, "df")) {
        os_sd_summary(a, sizeof(a));
        term_printf(t, "%s\n", a);
        os_usb_summary(a, sizeof(a));
        term_printf(t, "USB stick: %s\n", a);
    } else if (!strcmp(c, "eject")) {
        term_puts(t, usb_storage_eject() ? "The USB stick can be removed.\n" : "eject: no USB stick mounted\n");
    } else if (!strcmp(c, "ver")) {
        term_puts(t, OS_NAME " " OS_VERSION " on " OS_CHIP "\n");
    } else if (!strcmp(c, "uptime")) {
        uint32_t s = os_uptime_ms() / 1000;
        term_printf(t, "%u:%02u:%02u\n", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    } else if (!strcmp(c, "clear") || !strcmp(c, "cls")) {
        t->count = 1; t->lines[t->head][0] = 0;
    } else if (!strcmp(c, "exit")) {
        wm_close(t->win);
    } else {
        term_printf(t, "%s: unknown command. Type help for the list.\n", c);
    }
}

static void term_paint(widget_t *w, gfx_t *g)
{
    term_t *t = w->win->user;
    gfx_fill(g, 0, 0, w->r.w, w->r.h, C_BLACK);
    int rows = (w->r.h - 4) / 16;
    // the prompt line is drawn after the history
    char prompt[300];
    snprintf(prompt, sizeof(prompt), "%s> %.*s", term_show(t->cwd), t->ilen, t->input);
    int plen = strlen(prompt);
    int prows = t->scroll ? 0 : gfx_glyphs(prompt, plen) / T_COLS + 1;
    int hist = t->lines[t->head][0] ? t->count : t->count - 1;  // finished lines
    int show = rows - prows;
    int first = hist - show - t->scroll;
    if (first < 0) first = 0;
    int y = 2;
    for (int i = first; i < hist - t->scroll && i < first + show; i++, y += 16) {
        int idx = (t->head - (t->count - 1) + i + T_HIST * 2) % T_HIST;
        gfx_text(g, 4, y, t->lines[idx], -1, FONT_MONO, RGB222(2, 2, 2));
    }
    if (!t->scroll) {
        // the prompt wraps at glyph boundaries, so a line break never cuts an accented letter
        int off = 0;
        for (;;) {
            int gl = 0, pos = off;
            while (pos < plen && gl < T_COLS) { int b; gfx_glyph(prompt, pos, plen, &b); pos += b; gl++; }
            gfx_text(g, 4, y, prompt + off, pos - off, FONT_MONO, C_GREEN);
            if (pos >= plen && ui_focused(w) && ui_caret_on()) gfx_fill(g, 4 + gl * 8, y + 13, 8, 2, C_GREEN);
            y += 16;
            if (pos >= plen) break;
            off = pos;
        }
    }
}

static const char *const s_term_cmds[] = {
    "cat", "cd", "clear", "cls", "cp", "del", "df", "dir", "echo", "edit", "eject", "exit", "help", "ls", "mem",
    "mkdir", "mv", "open", "pwd", "rm", "rmdir", "run", "type", "uptime", "ver",
};

// Tab completes the word at the end of the line: a command for the first word, a file or folder
// name after it (spelt exactly, like everything else here). One match is filled in, with "/" after a
// folder; several are filled in as far as they agree, and if that adds nothing they are listed. A
// name with a space gets an opening quote, closed once a file is complete.
static void term_complete(term_t *t)
{
    t->input[t->ilen] = 0;
    int start = 0;
    bool quoted = false, first = true;
    for (int i = 0; i < t->ilen; i++) {
        if (t->input[i] == '"') { quoted = !quoted; if (quoted) start = i; }
        else if (t->input[i] == ' ' && !quoted) start = i + 1;
    }
    for (int i = 0; i < start; i++) if (t->input[i] != ' ') { first = false; break; }
    const char *word = t->input + start + (t->input[start] == '"');
    const char *slash = strrchr(word, '/');
    int dlen = slash ? (int)(slash - word) + 1 : 0;           // "Music/Live/" part, kept as typed
    const char *prefix = word + dlen;
    int plen = strlen(prefix);

    enum { MAXM = 64 };
    const char *match[MAXM];
    bool isdir[MAXM];
    int nm = 0;
    fs_entry_t *ents = NULL;
    if (first && !quoted) {
        for (int i = 0; i < (int)(sizeof(s_term_cmds) / sizeof(s_term_cmds[0])) && nm < MAXM; i++)
            if (!strncmp(s_term_cmds[i], prefix, plen)) { match[nm] = s_term_cmds[i]; isdir[nm++] = false; }
    } else {
        char dpart[160], dir[300];
        snprintf(dpart, sizeof(dpart), "%.*s", dlen, word);
        term_path(t, dpart, dir, sizeof(dir));
        int n = term_dir(dir) ? fs_list(dir, &ents) : -1;
        for (int i = 0; i < n && nm < MAXM; i++)
            if (!strncmp(ents[i].name, prefix, plen)) { match[nm] = ents[i].name; isdir[nm++] = ents[i].is_dir; }
    }
    if (nm) {
        // as far as all the matches agree
        int common = strlen(match[0]);
        for (int i = 1; i < nm; i++) {
            int k = 0;
            while (k < common && match[i][k] == match[0][k]) k++;
            common = k;
        }
        if (common > plen || nm == 1) {
            char out[sizeof(t->input)];
            bool space = memchr(match[0], ' ', common) != NULL || strchr(word, ' ');
            bool open_q = quoted || space;
            snprintf(out, sizeof(out), "%.*s%s%.*s%.*s%s", start, t->input, open_q ? "\"" : "", dlen, word,
                     common, match[0],
                     nm > 1 ? "" : isdir[0] ? "/" : open_q ? "\" " : " ");
            strlcpy(t->input, out, sizeof(t->input));
            t->ilen = strlen(t->input);
        } else {
            term_printf(t, "%s> %s\n", term_show(t->cwd), t->input);
            int col = 0;
            for (int i = 0; i < nm; i++) {
                int len = strlen(match[i]) + (isdir[i] ? 1 : 0);
                if (col && col + len + 2 > T_COLS) { term_puts(t, "\n"); col = 0; }
                term_printf(t, "%s%s%s", col ? "  " : "", match[i], isdir[i] ? "/" : "");
                col += len + (col ? 2 : 0);
            }
            term_puts(t, "\n");
        }
    }
    free(ents);
    t->scroll = 0;
}

static bool term_key(widget_t *w, const event_t *e)
{
    term_t *t = w->win->user;
    if (!e->down || (e->mods & MOD_ALT)) return false;
    if (e->mods & MOD_CTRL) {
        if (e->key == K_V) { int n; const char *c = clip_get(&n); for (int i = 0; i < n && t->ilen < (int)sizeof(t->input) - 1 && c[i] >= 32; i++) t->input[t->ilen++] = c[i]; }
        else if (e->key == K_L) { t->count = 1; t->lines[t->head][0] = 0; }
        else return false;
        ui_invalidate(w);
        return true;
    }
    switch (e->key) {
    case K_ENTER: case K_KP_ENTER: {
        t->input[t->ilen] = 0;
        t->scroll = 0;
        term_printf(t, "%s> %s\n", term_show(t->cwd), t->input);
        char cmd[160];
        strcpy(cmd, t->input);
        if (t->ilen) strcpy(t->last, t->input);
        t->ilen = 0;
        window_t *win = w->win;
        term_exec(t, cmd);
        if (!wm_valid(win) || (win->flags & WIN_DEAD)) return true;
        break;
    }
    case K_BKSP: if (t->ilen) t->ilen -= gfx_glyph_back(t->input, t->ilen); break;
    case K_UP: strcpy(t->input, t->last); t->ilen = strlen(t->input); break;
    case K_DOWN: t->ilen = 0; break;
    case K_PGUP: t->scroll += 10; if (t->scroll > t->count - 5) t->scroll = t->count > 5 ? t->count - 5 : 0; break;
    case K_PGDN: t->scroll -= 10; if (t->scroll < 0) t->scroll = 0; break;
    case K_ESC: t->ilen = 0; break;
    case K_TAB: if (e->mods & MOD_SHIFT) return false; term_complete(t); break;
    default:
        if (e->chs[0]) {
            int need = (int)strlen(e->chs);
            if (t->ilen + need >= (int)sizeof(t->input)) return false;
            memcpy(t->input + t->ilen, e->chs, need);
            t->ilen += need;
            t->scroll = 0;
        }
        else return false;
    }
    ui_invalidate(w);
    return true;
}

static void term_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    term_t *t = w->win->user;
    if (type != EV_WHEEL) return;
    t->scroll += btn * 3;
    if (t->scroll < 0) t->scroll = 0;
    if (t->scroll > t->count - 5) t->scroll = t->count > 5 ? t->count - 5 : 0;
    ui_invalidate(w);
}
static const canvas_ops_t term_ops = {term_paint, term_key, term_mouse};

static void term_resize(window_t *w)
{
    term_t *t = w->user;
    ui_set_rect(t->canvas, 0, 0, wm_client_w(w), wm_client_h(w));
}
static void term_destroy(window_t *w) { term_t *t = w->user; free(t->lines); free(t); }

void app_terminal(const char *arg)
{
    term_t *t = calloc(1, sizeof(term_t));
    t->lines = heap_caps_calloc(T_HIST, T_COLS + 1, MALLOC_CAP_SPIRAM);
    t->count = 1;
    strcpy(t->cwd, DISK_ROOT);
    window_t *w = wm_create("Terminal", T_COLS * 8 + 8, T_ROWS * 16 + 4, WIN_RESIZABLE);
    w->icon = &icon16_terminal;
    w->user = t;
    t->win = w;
    t->canvas = ui_canvas(w, 0, 0, T_COLS * 8 + 8, T_ROWS * 16 + 4, &term_ops, true);
    t->canvas->flags |= WF_WANT_CHARS | WF_WANT_ENTER | WF_WANT_ESC | WF_WANT_TAB | WF_CARET;
    t->canvas->flags &= ~WF_WANT_KEYUP;
    w->on_resize = term_resize;
    w->on_destroy = term_destroy;
    w->min_w = 300; w->min_h = 120;
    term_puts(t, OS_NAME " terminal. Type help for the commands.\n");
    wm_show(w);
}

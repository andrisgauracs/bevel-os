// Settings (saved to the drive), the picture viewer (BMP, JPEG and PNG) and the terminal.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tjpgd.h"
#include "miniz.h"

// ================================================================ settings

#define SETTINGS_FILE DISK_ROOT "/System/settings.ini"

static const struct { const char *name; uint8_t color; } s_colors[] = {
    {"Teal", RGB222_8(0, 2, 2)}, {"Navy", RGB222_8(0, 0, 2)}, {"Slate", RGB222_8(1, 1, 2)}, {"Forest", RGB222_8(0, 1, 0)},
    {"Plum", RGB222_8(1, 0, 1)}, {"Maroon", RGB222_8(1, 0, 0)}, {"Olive", RGB222_8(1, 1, 0)}, {"Storm", RGB222_8(0, 1, 1)},
    {"Grey", RGB222_8(1, 1, 1)}, {"Black", RGB222_8(0, 0, 0)}, {"Sky", RGB222_8(1, 2, 3)}, {"Sand", RGB222_8(2, 2, 1)},
};
#define NCOLORS ((int)(sizeof(s_colors) / sizeof(s_colors[0])))

#if BOARD_HAS_WALLPAPER
#define WALL_PATH_MAX 256
enum { WALL_FILL, WALL_FIT, WALL_CENTRE, WALL_STYLES };
static char s_wall_path[WALL_PATH_MAX];     // empty: no picture
static int s_wall_style;
static void wall_boot(void);
static void wall_settings_open(void);
static void wall_settings_commit(void);
static void wall_settings_cancel(void);
static void wall_settings_close(void);
static void wall_style_changed(int style);
static void wall_browse(widget_t *b, int code);
static void wall_none(widget_t *b, int code);
#endif

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
#if BOARD_HAS_WALLPAPER
        else if (!strcmp(line, "wallpaper")) strlcpy(s_wall_path, eq + 1, sizeof(s_wall_path));
        else if (!strcmp(line, "wallpaper_style") && v >= 0 && v < WALL_STYLES) s_wall_style = v;
#endif
    }
    free(s);
#if BOARD_HAS_WALLPAPER
    wall_boot();
#endif
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
#if BOARD_HAS_WALLPAPER
               "wallpaper=%s\nwallpaper_style=%d\n"
#endif
               "zone=%s\nlayout=%s\nremote=%d\n",
            shell_desktop_color, input_mouse_speed, input_repeat_delay, input_repeat_rate,
#if DISP_HAS_BACKLIGHT
            disp_get_brightness(),
#endif
#if BOARD_HAS_AUDIO
            audio_volume, audio_system_sounds, audio_output(),
#endif
#if BOARD_HAS_WALLPAPER
            s_wall_path, s_wall_style,
#endif
            clock_zone_name(clock_zone()), layout_name[input_layout_get()], remote_on());
    if (fclose(f) != 0) return false;
    unlink(SETTINGS_FILE);
    return rename(SETTINGS_FILE ".tmp", SETTINGS_FILE) == 0;
}

static window_t *s_set;
static struct {
    widget_t *colors, *speed, *delay, *rate, *bright, *volume, *output, *sounds, *zones, *layouts, *remote, *remote_url;
    widget_t *wall_name, *wall_style;
    uint8_t old_color; int old_speed, old_delay, old_rate, old_bright, old_volume, old_output, old_sounds, old_zone, old_layout;
    bool old_remote;
} s_sd;

static void colors_get(widget_t *lv, int i, lv_item_t *it) { it->text = s_colors[i].name; }

static void zones_get(widget_t *lv, int i, lv_item_t *it) { it->text = clock_zone_name(i); }

static void layouts_get(widget_t *lv, int i, lv_item_t *it) { it->text = layout_name[i]; }

#if BOARD_HAS_WALLPAPER
static void wall_styles_get(widget_t *lv, int i, lv_item_t *it)
{
    static const char *const names[WALL_STYLES] = {"Fill the screen", "Fit the screen", "Centre"};
    it->text = names[i];
}
#endif

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
#if BOARD_HAS_WALLPAPER
    wall_style_changed(ui_dropdown_sel(s_sd.wall_style));
#endif
    // preview swatch
    for (widget_t *x = s_set->widgets; x; x = x->next) if (!strcmp(x->cls->name, "canvas")) ui_invalidate(x);
}

static void swatch_paint(widget_t *w, gfx_t *g)
{
    gfx_fill(g, 0, 0, w->r.w, w->r.h, gfx_from8(shell_desktop_color));
    ui_draw_sunken(g, 0, 0, w->r.w, w->r.h);
    gfx_bitmap(g, (w->r.w - 32) / 2, 8, &icon32_folder);
    gfx_text(g, (w->r.w - gfx_text_w("Folder", -1, FONT_UI)) / 2, 44, "Folder", -1, FONT_UI, C_WHITE);
}
static const canvas_ops_t swatch_ops = {swatch_paint, NULL, NULL};

static void set_ok(widget_t *b, int code)
{
    set_apply(b, 0);
#if BOARD_HAS_WALLPAPER
    wall_settings_commit();
#endif
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
#if BOARD_HAS_WALLPAPER
    wall_settings_cancel();
#endif
    comp_invalidate_all();
    wm_destroy(s_set);
}

static bool set_close(window_t *w) { set_cancel(NULL, 0); return false; }
static void set_destroy(window_t *w)
{
    s_set = NULL;
#if BOARD_HAS_WALLPAPER
    wall_settings_close();
#endif
}

void app_settings(const char *arg)
{
    if (s_set) { wm_activate(s_set); return; }
    // the groups end at x = 630: on the S3's 640-pixel screen the client area is 632 wide, so the
    // frame fits the screen exactly instead of hanging 8 pixels off its right edge
    int sw = SCREEN_W - 2 * BORDER_W < 640 ? SCREEN_W - 2 * BORDER_W : 640;
    int buttons_y = 302;
#if BOARD_HAS_WALLPAPER
    buttons_y += 56;                    // the desktop picture has a row of its own
#endif
    window_t *w = wm_create("Settings", sw, buttons_y + 32, WIN_DIALOG);
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

#if BOARD_HAS_WALLPAPER
    wall_settings_open();
    ui_groupbox(w, 10, 302, 620, 50, "Desktop picture");
    s_sd.wall_name = ui_label(w, 22, 325, 228, 14, s_wall_path[0] ? fs_basename(s_wall_path) : "None");
    ui_button(w, 256, 320, 78, 24, "&Picture...", wall_browse);
    ui_button(w, 338, 320, 56, 24, "&None", wall_none);
    ui_label(w, 406, 325, 38, 14, "S&tyle:");
    s_sd.wall_style = ui_dropdown(w, 446, 321, 172, wall_styles_get);
    ui_dropdown_set_count(s_sd.wall_style, WALL_STYLES);
    ui_dropdown_select(s_sd.wall_style, s_wall_style);
    s_sd.wall_style->on_action = set_apply;
#endif

    widget_t *ok = ui_button(w, 468, buttons_y, 78, 24, "OK", set_ok);
    widget_t *ca = ui_button(w, 552, buttons_y, 78, 24, "Cancel", set_cancel);
    w->def_btn = ok; w->cancel_btn = ca;
    w->on_close = set_close;
    w->on_destroy = set_destroy;
    w->focus = s_sd.colors;
    wm_show(w);
}

// ================================================================ picture viewer

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

#define PIC_MAX 2048            // the largest picture kept in memory, each way

// Where a decoder puts its pixels, a row at a time from the top: true colour where the screen has
// it, otherwise RGB222 with Floyd-Steinberg dithering.
typedef struct {
    pixel_t *out;
    int w;
    int16_t *e0, *e1;           // dither error for this row and the next
} pic_sink_t;

static void sink_free_dither(pic_sink_t *s)
{
    free(s->e0); free(s->e1);
    s->e0 = s->e1 = NULL;
}

static bool sink_open(pic_sink_t *s, int w, int h)
{
    s->w = w;
    s->out = heap_caps_malloc((size_t)w * h * sizeof(pixel_t), MALLOC_CAP_SPIRAM);
    s->e0 = heap_caps_calloc((w + 2) * 3, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->e1 = heap_caps_calloc((w + 2) * 3, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s->out && s->e0 && s->e1) return true;
    free(s->out);
    s->out = NULL;
    sink_free_dither(s);
    return false;
}

static inline void sink_row_start(pic_sink_t *s) { memset(s->e1, 0, (s->w + 2) * 3 * sizeof(int16_t)); }
static inline void sink_row_end(pic_sink_t *s) { int16_t *t = s->e0; s->e0 = s->e1; s->e1 = t; }

static inline void sink_pixel(pic_sink_t *s, pixel_t *d, int x, int r, int g, int b)
{
#if DISP_TRUE_COLOR
    pixel_t p = RGB565(r, g, b);
    if ((p & ~(pixel_t)(DISP_OVERLAYS - 1)) == DISP_OVERLAY_KEY_BASE) p ^= 0x20;    // never an overlay's key
    d[x] = p;
#else
    const int c[3] = {r, g, b};
    uint8_t pix = 0;
    for (int k = 0; k < 3; k++) {
        int v = c[k] + s->e0[(x + 1) * 3 + k] / 16;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        int q = (v + 42) / 85;          // nearest of 0, 85, 170, 255
        int err_v = v - q * 85;
        pix |= q << (k * 2);
        s->e0[(x + 2) * 3 + k] += err_v * 7;
        s->e1[x * 3 + k] += err_v * 3;
        s->e1[(x + 1) * 3 + k] += err_v * 5;
        s->e1[(x + 2) * 3 + k] += err_v;
    }
    d[x] = pix;
#endif
}

// Load a BMP into screen pixels. Returns NULL and sets *err.
static pixel_t *bmp_load(const char *path, int *out_w, int *out_h, const char **err)
{
    FILE *f = fopen(path, "rb");
    *err = "The file could not be opened.";
    if (!f) return NULL;
    uint8_t hdr[54];
    pic_sink_t sink = {0};
    uint8_t *row = NULL;
    uint8_t pal[256][4];
    *err = "This is not a BMP, JPEG or PNG picture.";
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
    if (w <= 0 || h <= 0 || w > PIC_MAX || h > PIC_MAX) goto done;
    if (bpp <= 8) {
        int ncol = rd32(hdr + 46);
        if (ncol <= 0 || ncol > 256) ncol = 1 << bpp;
        fseek(f, 14 + hsize, SEEK_SET);
        memset(pal, 0, sizeof(pal));
        if (fread(pal, 4, ncol, f) != (size_t)ncol) { *err = "The picture file is damaged."; goto done; }
    }
    int stride = ((w * bpp + 31) / 32) * 4;
    *err = "Not enough memory for this picture.";
    row = heap_caps_malloc(stride, MALLOC_CAP_SPIRAM);
    if (!row || !sink_open(&sink, w, h)) goto done;
    for (int y = 0; y < h; y++) {
        // rows are stored bottom-up; read them in display order so the dither runs top-down
        int file_row = flip ? h - 1 - y : y;
        fseek(f, data_off + (long)file_row * stride, SEEK_SET);
        if (fread(row, 1, stride, f) != (size_t)stride) { *err = "The picture file is damaged."; free(sink.out); sink.out = NULL; goto done; }
        sink_row_start(&sink);
        pixel_t *d = sink.out + y * w;
        for (int x = 0; x < w; x++) {
            if (bpp >= 24) { const uint8_t *p = row + x * (bpp / 8); sink_pixel(&sink, d, x, p[2], p[1], p[0]); }
            else {
                int idx = bpp == 8 ? row[x] : bpp == 4 ? (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15 : (row[x >> 3] >> (7 - (x & 7))) & 1;
                sink_pixel(&sink, d, x, pal[idx][2], pal[idx][1], pal[idx][0]);
            }
        }
        sink_row_end(&sink);
    }
    *out_w = w; *out_h = h;
    *err = NULL;
done:
    free(row);
    sink_free_dither(&sink);
    fclose(f);
    return sink.out;
}

// A big picture takes many seconds to decode: the idle task's watchdog must still get to run.
static void pic_yield(uint32_t *last)
{
    if (os_uptime_ms() - *last < 500) return;
    vTaskDelay(1);
    *last = os_uptime_ms();
}

// A JPEG is decoded straight from the file, one row of blocks at a time, so a photo far larger than
// memory can still be opened: the decoder shrinks it by 2, 4 or 8 on the way, as much as it takes to
// fit PIC_MAX and the free memory. Baseline JPEG only, which is what TJpgDec reads.
#define JPG_POOL (24 * 1024)    // decoder tables and its 4 kB read buffer (CONFIG_JD_SZBUF)

typedef struct {
    FILE *f;
    pic_sink_t sink;
    uint8_t *strip;             // RGB888, one row of blocks across the picture
    uint32_t yielded;
} jpg_t;

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jpg_t *j = jd->device;
    if (buf) return fread(buf, 1, n, j->f);
    return fseek(j->f, n, SEEK_CUR) ? 0 : n;
}

// Blocks arrive left to right; the last one of a row sends the whole row on, top to bottom, which is
// the order the dither needs.
static int jpg_out(JDEC *jd, void *px, JRECT *r)
{
    jpg_t *j = jd->device;
    int w = j->sink.w, rw = r->right - r->left + 1, rows = r->bottom - r->top + 1;
    const uint8_t *src = px;
    for (int y = 0; y < rows; y++) memcpy(j->strip + (y * w + r->left) * 3, src + y * rw * 3, rw * 3);
    if (r->right != w - 1) return 1;
    for (int y = 0; y < rows; y++) {
        const uint8_t *p = j->strip + y * w * 3;
        pixel_t *d = j->sink.out + (r->top + y) * w;
        sink_row_start(&j->sink);
        for (int x = 0; x < w; x++, p += 3) sink_pixel(&j->sink, d, x, p[0], p[1], p[2]);
        sink_row_end(&j->sink);
    }
    pic_yield(&j->yielded);
    return 1;
}

static const char *jpg_error(JRESULT r)
{
    if (r == JDR_INP || r == JDR_FMT1) return "The picture file is damaged.";
    return "This kind of JPEG is not supported (progressive and CMYK pictures cannot be opened).";
}

static pixel_t *jpg_load(const char *path, int *out_w, int *out_h, const char **err)
{
    FILE *f = fopen(path, "rb");
    *err = "The file could not be opened.";
    if (!f) return NULL;
    jpg_t j = {.f = f, .yielded = os_uptime_ms()};
    JDEC jd;
    void *pool = heap_caps_malloc(JPG_POOL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pool) pool = heap_caps_malloc(JPG_POOL, MALLOC_CAP_SPIRAM);
    *err = "Not enough memory for this picture.";
    if (!pool) goto done;
    JRESULT r = jd_prepare(&jd, jpg_in, pool, JPG_POOL, &j);
    if (r != JDR_OK) { *err = jpg_error(r); goto done; }
    *err = "The picture file is damaged.";
    if (!jd.width || !jd.height) goto done;
    int scale = 0;
    while (scale < 3 && ((jd.width >> scale) > PIC_MAX || (jd.height >> scale) > PIC_MAX)) scale++;
    *err = "The picture is too large (the limit is 16384 x 16384).";
    if ((jd.width >> scale) > PIC_MAX || (jd.height >> scale) > PIC_MAX) goto done;
    *err = "Not enough memory for this picture.";
    int w, h;
    for (;; scale++) {
        if (scale > 3) goto done;
        w = jd.width >> scale;
        h = jd.height >> scale;
        if (!w || !h) goto done;
        if (!sink_open(&j.sink, w, h)) continue;
        j.strip = heap_caps_malloc((size_t)w * 3 * ((jd.msy * 8) >> scale), MALLOC_CAP_SPIRAM);
        if (j.strip) break;
        free(j.sink.out);
        j.sink.out = NULL;
        sink_free_dither(&j.sink);
    }
    r = jd_decomp(&jd, jpg_out, scale);
    if (r != JDR_OK) { *err = jpg_error(r); free(j.sink.out); j.sink.out = NULL; goto done; }
    *out_w = w; *out_h = h;
    *err = NULL;
done:
    free(j.strip);
    sink_free_dither(&j.sink);
    free(pool);
    fclose(f);
    return j.sink.out;
}

// A PNG is inflated straight from the file by the decompressor in ROM, a row at a time. Every PNG
// kind is read: all colour types and bit depths (16-bit samples keep their high byte), interlacing,
// and transparency, which is blended over the viewer's background. A picture beyond PIC_MAX or the
// free memory is shrunk by 2, 4 or 8, as a JPEG is.
#define PNG_BG 85               // the grey behind transparent parts (C_DKGREY)

typedef struct {
    FILE *f;
    pic_sink_t sink;
    uint32_t yielded;
    int w, h, depth, ctype, channels;
    bool interlaced, has_key, inflate_done, no_more_idat;
    uint16_t key[3];            // the sample values tRNS makes transparent (grey and RGB pictures)
    uint32_t chunk_left;        // bytes of the current IDAT not read yet
    size_t in_pos, in_len, out_pos, out_len, dict_pos;
    tinfl_decompressor inflate;
    uint8_t palette[256][3], alpha[256];
    uint8_t in[4096];
    uint8_t dict[TINFL_LZ_DICT_SIZE];
} png_t;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static bool png_chunk_header(png_t *p, uint32_t *len, uint8_t type[4])
{
    uint8_t h[8];
    if (fread(h, 1, 8, p->f) != 8) return false;
    *len = be32(h);
    memcpy(type, h + 4, 4);
    return *len <= 0x7FFFFFFF;
}

// The next compressed bytes, which may run on through any number of IDAT chunks.
static bool png_fill(png_t *p)
{
    while (!p->chunk_left) {
        uint8_t type[4];
        uint32_t len;
        if (p->no_more_idat || fseek(p->f, 4, SEEK_CUR) || !png_chunk_header(p, &len, type) || memcmp(type, "IDAT", 4)) {
            p->no_more_idat = true;
            return false;
        }
        p->chunk_left = len;
    }
    size_t want = p->chunk_left < sizeof(p->in) ? p->chunk_left : sizeof(p->in);
    size_t got = fread(p->in, 1, want, p->f);
    p->in_pos = 0;
    p->in_len = got;
    if (!got) { p->chunk_left = 0; p->no_more_idat = true; return false; }
    p->chunk_left -= got;
    return true;
}

static bool png_read(png_t *p, uint8_t *dst, size_t n)
{
    while (n) {
        if (p->out_len) {
            size_t k = p->out_len < n ? p->out_len : n;
            memcpy(dst, p->dict + p->out_pos, k);
            dst += k; n -= k;
            p->out_pos += k; p->out_len -= k;
            continue;
        }
        if (p->inflate_done) return false;
        if (p->in_pos == p->in_len) png_fill(p);
        bool more = p->chunk_left || !p->no_more_idat;
        size_t in_n = p->in_len - p->in_pos, out_n = sizeof(p->dict) - p->dict_pos;
        tinfl_status st = tinfl_decompress(&p->inflate, p->in + p->in_pos, &in_n, p->dict, p->dict + p->dict_pos, &out_n,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER | (more ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        p->in_pos += in_n;
        p->out_pos = p->dict_pos;
        p->out_len = out_n;
        p->dict_pos = (p->dict_pos + out_n) & (sizeof(p->dict) - 1);
        if (st < 0) return false;
        if (st == TINFL_STATUS_DONE) p->inflate_done = true;
        else if (!in_n && !out_n && !more) return false;
    }
    return true;
}

// One row with its filter undone. prev is the row above, all zero at the start of a pass.
static bool png_row(png_t *p, uint8_t *cur, const uint8_t *prev, int stride, int bpp)
{
    uint8_t filter;
    if (!png_read(p, &filter, 1) || !png_read(p, cur, stride) || filter > 4) return false;
    switch (filter) {
    case 1: for (int i = bpp; i < stride; i++) cur[i] += cur[i - bpp]; break;
    case 2: for (int i = 0; i < stride; i++) cur[i] += prev[i]; break;
    case 3:
        for (int i = 0; i < stride; i++) cur[i] += ((i >= bpp ? cur[i - bpp] : 0) + prev[i]) / 2;
        break;
    case 4:
        for (int i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
            cur[i] += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
        }
        break;
    }
    return true;
}

// Sample number i of a row, at the picture's bit depth.
static inline int png_sample(const png_t *p, const uint8_t *row, int i)
{
    int d = p->depth;
    if (d == 8) return row[i];
    if (d == 16) return row[i * 2] << 8 | row[i * 2 + 1];
    int bit = i * d;
    return (row[bit >> 3] >> (8 - d - (bit & 7))) & ((1 << d) - 1);
}

static inline int png_to8(const png_t *p, int v)
{
    return p->depth == 16 ? v >> 8 : p->depth == 8 ? v : v * 255 / ((1 << p->depth) - 1);
}

// Pixel x of a row as RGB, blended over the background.
static inline void png_pixel(const png_t *p, const uint8_t *row, int x, uint8_t *rgb)
{
    int a = 255, n = p->channels;
    switch (p->ctype) {
    case 3: {
        int i = png_sample(p, row, x);
        memcpy(rgb, p->palette[i], 3);
        a = p->alpha[i];
        break;
    }
    case 0: case 4: {
        int v = png_sample(p, row, x * n);
        rgb[0] = rgb[1] = rgb[2] = png_to8(p, v);
        if (n == 2) a = png_to8(p, png_sample(p, row, x * n + 1));
        else if (p->has_key && v == p->key[0]) a = 0;
        break;
    }
    default: {
        int r = png_sample(p, row, x * n), g = png_sample(p, row, x * n + 1), b = png_sample(p, row, x * n + 2);
        rgb[0] = png_to8(p, r); rgb[1] = png_to8(p, g); rgb[2] = png_to8(p, b);
        if (n == 4) a = png_to8(p, png_sample(p, row, x * n + 3));
        else if (p->has_key && r == p->key[0] && g == p->key[1] && b == p->key[2]) a = 0;
        break;
    }
    }
    if (a != 255) for (int k = 0; k < 3; k++) rgb[k] = (rgb[k] * a + PNG_BG * (255 - a)) / 255;
}

// Everything before the first IDAT. Leaves the file at that chunk's data.
static const char *png_header(png_t *p)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    const char *damaged = "The picture file is damaged.";
    uint8_t h[8 + 13], type[4];
    uint32_t len;
    if (fread(h, 1, 8, p->f) != 8 || memcmp(h, sig, 8)) return damaged;
    if (!png_chunk_header(p, &len, type) || memcmp(type, "IHDR", 4) || len != 13 || fread(h, 1, 13, p->f) != 13) return damaged;
    uint32_t w = be32(h), hh = be32(h + 4);
    int d = h[8], c = h[9];
    bool depth_ok = c == 0 ? d == 1 || d == 2 || d == 4 || d == 8 || d == 16
                  : c == 3 ? d == 1 || d == 2 || d == 4 || d == 8
                  : (c == 2 || c == 4 || c == 6) && (d == 8 || d == 16);
    if (!w || !hh || !depth_ok || h[10] || h[11] || h[12] > 1) return damaged;
    if (w > PIC_MAX << 3 || hh > PIC_MAX << 3) return "The picture is too large (the limit is 16384 x 16384).";
    p->w = w; p->h = hh; p->depth = d; p->ctype = c; p->interlaced = h[12];
    p->channels = c == 2 ? 3 : c == 4 ? 2 : c == 6 ? 4 : 1;
    memset(p->alpha, 255, sizeof(p->alpha));
    bool have_palette = false;
    fseek(p->f, 4, SEEK_CUR);
    for (;;) {
        if (!png_chunk_header(p, &len, type)) return damaged;
        if (!memcmp(type, "IDAT", 4)) {
            if (c == 3 && !have_palette) return damaged;
            p->chunk_left = len;
            return NULL;
        }
        if (!memcmp(type, "IEND", 4)) return damaged;
        uint32_t used = 0;
        if (!memcmp(type, "PLTE", 4) && len <= sizeof(p->palette)) {
            if (fread(p->palette, 1, len, p->f) != len) return damaged;
            used = len;
            have_palette = true;
        } else if (!memcmp(type, "tRNS", 4)) {
            uint8_t k[6];
            if (c == 3 && len <= sizeof(p->alpha)) {
                if (fread(p->alpha, 1, len, p->f) != len) return damaged;
                used = len;
            } else if ((c == 0 && len == 2) || (c == 2 && len == 6)) {
                if (fread(k, 1, len, p->f) != len) return damaged;
                used = len;
                for (uint32_t i = 0; i < len / 2; i++) p->key[i] = k[i * 2] << 8 | k[i * 2 + 1];
                p->has_key = true;
            }
        }
        if (fseek(p->f, (long)(len - used) + 4, SEEK_CUR)) return damaged;
    }
}

static pixel_t *png_load(const char *path, int *out_w, int *out_h, const char **err)
{
    // the seven passes of an interlaced picture: start and step, across and down
    static const uint8_t adam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    static const uint8_t whole[1][4] = {{0, 0, 1, 1}};
    FILE *f = fopen(path, "rb");
    *err = "The file could not be opened.";
    if (!f) return NULL;
    png_t *p = heap_caps_calloc(1, sizeof(png_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(1, sizeof(png_t), MALLOC_CAP_SPIRAM);
    uint8_t *cur = NULL, *prev = NULL, *canvas = NULL;
    uint16_t *sum = NULL;
    pixel_t *out = NULL;
    *err = "Not enough memory for this picture.";
    if (!p) goto done;
    p->f = f;
    p->yielded = os_uptime_ms();
    tinfl_init(&p->inflate);
    *err = png_header(p);
    if (*err) goto done;

    int scale = 0;
    while (scale < 3 && ((p->w >> scale) > PIC_MAX || (p->h >> scale) > PIC_MAX)) scale++;
    int bits = p->channels * p->depth, bpp = (bits + 7) / 8;
    size_t row_size = ((size_t)p->w * bits + 7) / 8;
    int w, h;
    *err = "Not enough memory for this picture.";
    for (;; scale++) {
        if (scale > 3) goto done;
        w = p->w >> scale;
        h = p->h >> scale;
        if (!w || !h) goto done;
        if (!sink_open(&p->sink, w, h)) continue;
        cur = heap_caps_malloc(row_size, MALLOC_CAP_SPIRAM);
        prev = heap_caps_malloc(row_size, MALLOC_CAP_SPIRAM);
        // straight through, the rows of a block are averaged as they pass; the passes of an
        // interlaced picture arrive out of order and have to be collected first
        if (p->interlaced) canvas = heap_caps_malloc((size_t)w * h * 3, MALLOC_CAP_SPIRAM);
        else sum = heap_caps_malloc((size_t)w * 3 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (cur && prev && (canvas || sum)) break;
        free(cur); free(prev); free(canvas); free(sum);
        cur = prev = canvas = NULL;
        sum = NULL;
        free(p->sink.out);
        p->sink.out = NULL;
        sink_free_dither(&p->sink);
    }

    int block = 1 << scale, out_y = 0;
    if (sum) memset(sum, 0, (size_t)w * 3 * sizeof(uint16_t));
    const uint8_t (*passes)[4] = p->interlaced ? adam7 : whole;
    for (int pass = 0; pass < (p->interlaced ? 7 : 1); pass++) {
        int x0 = passes[pass][0], y0 = passes[pass][1], dx = passes[pass][2], dy = passes[pass][3];
        int pw = p->w > x0 ? (p->w - x0 + dx - 1) / dx : 0, ph = p->h > y0 ? (p->h - y0 + dy - 1) / dy : 0;
        if (!pw || !ph) continue;
        int stride = ((size_t)pw * bits + 7) / 8;
        memset(prev, 0, stride);
        for (int r = 0; r < ph; r++) {
            if (!png_row(p, cur, prev, stride, bpp)) { *err = "The picture file is damaged."; goto fail; }
            int y = y0 + r * dy, oy = y >> scale;
            if (oy < h) {
                for (int c = 0; c < pw; c++) {
                    int ox = (x0 + c * dx) >> scale;
                    if (ox >= w) break;
                    uint8_t rgb[3];
                    png_pixel(p, cur, c, rgb);
                    if (canvas) memcpy(canvas + ((size_t)oy * w + ox) * 3, rgb, 3);
                    else for (int k = 0; k < 3; k++) sum[ox * 3 + k] += rgb[k];
                }
                if (sum && (y & (block - 1)) == block - 1) {
                    int div = block * block;
                    pixel_t *d = p->sink.out + out_y++ * w;
                    sink_row_start(&p->sink);
                    for (int x = 0; x < w; x++) sink_pixel(&p->sink, d, x, sum[x * 3] / div, sum[x * 3 + 1] / div, sum[x * 3 + 2] / div);
                    sink_row_end(&p->sink);
                    memset(sum, 0, (size_t)w * 3 * sizeof(uint16_t));
                }
            }
            uint8_t *t = cur; cur = prev; prev = t;
            pic_yield(&p->yielded);
        }
    }
    if (canvas) {
        for (int y = 0; y < h; y++) {
            const uint8_t *s = canvas + (size_t)y * w * 3;
            pixel_t *d = p->sink.out + y * w;
            sink_row_start(&p->sink);
            for (int x = 0; x < w; x++, s += 3) sink_pixel(&p->sink, d, x, s[0], s[1], s[2]);
            sink_row_end(&p->sink);
        }
    }
    out = p->sink.out;
    p->sink.out = NULL;
    *out_w = w; *out_h = h;
    *err = NULL;
fail:
    free(p->sink.out);
    sink_free_dither(&p->sink);
done:
    free(cur); free(prev); free(canvas); free(sum);
    free(p);
    fclose(f);
    return out;
}

// The file's first bytes say what it is, whatever it is called.
static pixel_t *pic_load(const char *path, int *out_w, int *out_h, const char **err)
{
    uint8_t magic[4] = {0};
    FILE *f = fopen(path, "rb");
    if (f) { fread(magic, 1, 4, f); fclose(f); }
    if (magic[0] == 0xFF && magic[1] == 0xD8) return jpg_load(path, out_w, out_h, err);
    if (!memcmp(magic, "\x89PNG", 4)) return png_load(path, out_w, out_h, err);
    return bmp_load(path, out_w, out_h, err);
}

typedef struct {
    window_t *win;
    widget_t *canvas;
    pixel_t *px;
    int w, h, sx, sy;
    int zoom;                   // screen pixels per picture pixel, in 1/256: 256 is actual size
    bool drag;
    int dx, dy, drag_sx, drag_sy;   // where the drag began: pointer, and scroll position
    int pinch_zoom, pinch_x, pinch_y;   // at the start of a pinch: zoom, picture point under the fingers
    bool fit_on, fit_maximized; // fit_maximized: turning fit on is what maximised the window
    pic_sink_t fit;             // the scaled picture, in rows of SCREEN_W pixels
    int fit_w, fit_h;           // the size it was last scaled to
} viewer_t;

static inline void pixel_rgb(pixel_t p, int *r, int *g, int *b)
{
#if DISP_TRUE_COLOR
    int r5 = p >> 11, g6 = (p >> 5) & 63, b5 = p & 31;
    *r = r5 << 3 | r5 >> 2;
    *g = g6 << 2 | g6 >> 4;
    *b = b5 << 3 | b5 >> 2;
#else
    *r = (p & 3) * 85;
    *g = ((p >> 2) & 3) * 85;
    *b = ((p >> 4) & 3) * 85;
#endif
}

// The part (cx, cy, cw, ch) of a picture, scaled to fw x fh into rows of `stride` pixels. Each new
// pixel is the average of up to 4 x 4 samples of what it covers, which also evens out the S3's dither
// before the result is dithered again.
static void pic_scale(pic_sink_t *s, int stride, const pixel_t *src, int src_w, int cx, int cy, int cw, int ch, int fw, int fh)
{
    s->w = fw;
    memset(s->e0, 0, (fw + 2) * 3 * sizeof(int16_t));
    for (int y = 0; y < fh; y++) {
        int y0 = cy + y * ch / fh, y1 = cy + (y + 1) * ch / fh;
        if (y1 <= y0) y1 = y0 + 1;
        int ystep = (y1 - y0 + 3) / 4;
        pixel_t *d = s->out + y * stride;
        sink_row_start(s);
        for (int x = 0; x < fw; x++) {
            int x0 = cx + x * cw / fw, x1 = cx + (x + 1) * cw / fw;
            if (x1 <= x0) x1 = x0 + 1;
            int xstep = (x1 - x0 + 3) / 4;
            int r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy += ystep) {
                const pixel_t *row = src + sy * src_w;
                for (int sx = x0; sx < x1; sx += xstep, n++) {
                    int cr, cg, cb;
                    pixel_rgb(row[sx], &cr, &cg, &cb);
                    r += cr; g += cg; b += cb;
                }
            }
            sink_pixel(s, d, x, r / n, g / n, b / n);
        }
        sink_row_end(s);
    }
}

// ================================================================ desktop picture

#if BOARD_HAS_WALLPAPER
// The picture is kept scaled to the desktop, so painting is a copy and the original is not needed.
// At boot a task decodes the original while the desktop comes up, so a large photo costs no start-up
// time. Nothing is stored on the drive: writing a screenful there takes some 20 s and stalls the
// system meanwhile. If the file is gone at boot, the desktop simply shows its colour.
#define WALL_WAIT_MS 15000      // a USB stick is mounted some seconds after the settings are read

typedef struct { pixel_t *px; int x, y, w, h; } wall_t;

static wall_t s_wall;

// What Settings needs while it is open: how to go back, and the decoded original, so that trying
// the styles does not decode it each time.
static struct {
    bool replaced;              // s_wall is a new picture and `old` the one to go back to
    wall_t old;
    char old_path[WALL_PATH_MAX];
    int old_style;
    pixel_t *src;
    int src_w, src_h;
    char src_path[WALL_PATH_MAX];
} s_ws;

void wallpaper_paint(gfx_t *g, pixel_t background)
{
    const wall_t *w = &s_wall;
    if (!w->px || w->w < SCREEN_W || w->h < DESK_H) gfx_fill(g, 0, 0, SCREEN_W, DESK_H, background);
    if (w->px) gfx_blit(g, w->x, w->y, w->w, w->h, w->px, w->w);
}

static bool wall_compose(const pixel_t *src, int sw, int sh, int style, wall_t *out)
{
    int dw = SCREEN_W, dh = DESK_H, cx = 0, cy = 0, cw = sw, ch = sh, fw = dw, fh = dh;
    if (style == WALL_FILL) {                   // covers the desktop, the excess cut off evenly
        if (sw * dh > sh * dw) { cw = sh * dw / dh; cx = (sw - cw) / 2; }
        else { ch = sw * dh / dw; cy = (sh - ch) / 2; }
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
    } else if (style == WALL_FIT) {             // all of it, the colour showing around it
        if (sw * dh >= sh * dw) fh = sh * dw / sw; else fw = sw * dh / sh;
        if (fw < 1) fw = 1;
        if (fh < 1) fh = 1;
    } else {                                    // actual size
        fw = cw = sw < dw ? sw : dw;
        fh = ch = sh < dh ? sh : dh;
        cx = (sw - cw) / 2;
        cy = (sh - ch) / 2;
    }
    pic_sink_t sink = {0};
    if (!sink_open(&sink, fw, fh)) return false;
    pic_scale(&sink, fw, src, sw, cx, cy, cw, ch, fw, fh);
    sink_free_dither(&sink);
    *out = (wall_t){sink.out, (dw - fw) / 2, (dh - fh) / 2, fw, fh};
    return true;
}

static void wall_redraw(void) { comp_invalidate(0, 0, SCREEN_W, DESK_H); }

static void wall_install(void *arg)
{
    wall_t *w = arg;
    if (s_wall.px || s_ws.replaced) free(w->px);    // Settings got there first
    else { s_wall = *w; wall_redraw(); }
    free(w);
}

static void wall_boot_task(void *arg)
{
    wall_t w = {0};
    uint32_t t0 = os_uptime_ms();
    while (!fs_exists(s_wall_path) && os_uptime_ms() - t0 < WALL_WAIT_MS) vTaskDelay(pdMS_TO_TICKS(250));
    const char *err;
    int sw = 0, sh = 0;
    pixel_t *src = pic_load(s_wall_path, &sw, &sh, &err);
    if (src) {
        if (!wall_compose(src, sw, sh, s_wall_style, &w)) w.px = NULL;
        free(src);
    }
    wall_t *h = w.px ? malloc(sizeof(wall_t)) : NULL;
    if (h) { *h = w; ev_call(wall_install, h); }
    else free(w.px);
    vTaskDelete(NULL);
}

static void wall_boot(void)
{
    if (s_wall_path[0]) xTaskCreatePinnedToCore(wall_boot_task, "wallpaper", 8192, NULL, 2, NULL, SYS_CORE);
}

static void wall_settings_open(void)
{
    memset(&s_ws, 0, sizeof(s_ws));
    strlcpy(s_ws.old_path, s_wall_path, sizeof(s_ws.old_path));
    s_ws.old_style = s_wall_style;
}

static void wall_show(const wall_t *w, const char *path, int style)
{
    if (s_ws.replaced) free(s_wall.px);
    else { s_ws.replaced = true; s_ws.old = s_wall; }
    s_wall = *w;
    if (path != s_wall_path) strlcpy(s_wall_path, path, sizeof(s_wall_path));
    s_wall_style = style;
    ui_set_text(s_sd.wall_name, path[0] ? fs_basename(path) : "None");
    wall_redraw();
}

// Shows a picture in a style, live like the colour. False if it could not be done.
static bool wall_try(const char *path, int style)
{
    const char *err = NULL;
    wall_t w = {0};
    wm_busy(true);
    if (!s_ws.src || strcmp(s_ws.src_path, path)) {
        int sw = 0, sh = 0;
        pixel_t *src = pic_load(path, &sw, &sh, &err);
        if (src) {
            free(s_ws.src);
            s_ws.src = src; s_ws.src_w = sw; s_ws.src_h = sh;
            strlcpy(s_ws.src_path, path, sizeof(s_ws.src_path));
        }
    }
    if (!err && !wall_compose(s_ws.src, s_ws.src_w, s_ws.src_h, style, &w)) err = "Not enough memory for this picture.";
    wm_busy(false);
    if (err) { msgbox(s_set, "Desktop picture", err, MB_ERROR, MB_OK); return false; }
    wall_show(&w, path, style);
    return true;
}

static void wall_browse(widget_t *b, int code)
{
    char path[WALL_PATH_MAX];
    strlcpy(path, s_wall_path[0] ? s_wall_path : DISK_ROOT "/Pictures", sizeof(path));
    if (file_dialog(s_set, false, "Desktop Picture", path, sizeof(path))) wall_try(path, s_wall_style);
}

static void wall_none(widget_t *b, int code)
{
    if (!s_wall_path[0]) return;
    wall_t none = {0};
    wall_show(&none, "", s_wall_style);
}

static void wall_style_changed(int style)
{
    if (style < 0 || style == s_wall_style) return;
    if (!s_wall_path[0]) s_wall_style = style;
    else if (!wall_try(s_wall_path, style)) ui_dropdown_select(s_sd.wall_style, s_wall_style);
}

static void wall_settings_commit(void)
{
    if (!s_ws.replaced) return;
    free(s_ws.old.px);
    s_ws.old.px = NULL;
    s_ws.replaced = false;
}

static void wall_settings_cancel(void)
{
    if (s_ws.replaced) { free(s_wall.px); s_wall = s_ws.old; s_ws.replaced = false; }
    strlcpy(s_wall_path, s_ws.old_path, sizeof(s_wall_path));
    s_wall_style = s_ws.old_style;
}

static void wall_settings_close(void)
{
    free(s_ws.src);
    s_ws.src = NULL;
}
#endif

// ================================================================ picture viewer, continued

#define VIEW_ZOOM_MAX (4 * 256)

static void view_clamp(viewer_t *v)
{
    int vw = v->canvas->r.w * 256 / v->zoom, vh = v->canvas->r.h * 256 / v->zoom;
    if (v->sx > v->w - vw) v->sx = v->w - vw;
    if (v->sy > v->h - vh) v->sy = v->h - vh;
    if (v->sx < 0) v->sx = 0;
    if (v->sy < 0) v->sy = 0;
}

// The largest size with the picture's proportions that fits the canvas.
static void view_fit_size(const viewer_t *v, int *fw, int *fh)
{
    int cw = v->canvas->r.w < SCREEN_W ? v->canvas->r.w : SCREEN_W;
    int ch = v->canvas->r.h < SCREEN_H ? v->canvas->r.h : SCREEN_H;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (v->w * ch >= v->h * cw) { *fw = cw; *fh = v->h * cw / v->w; }
    else { *fh = ch; *fw = v->w * ch / v->h; }
    if (*fw < 1) *fw = 1;
    if (*fh < 1) *fh = 1;
}

// The zoom that shows the whole picture in the canvas.
static int view_fit_zoom(const viewer_t *v)
{
    int fw, fh;
    view_fit_size(v, &fw, &fh);
    int z = fw * 256 / v->w;
    return z < 1 ? 1 : z;
}

static void view_fit_render(viewer_t *v, int fw, int fh)
{
    pic_scale(&v->fit, SCREEN_W, v->px, v->w, 0, 0, v->w, v->h, fw, fh);
    v->fit_w = fw;
    v->fit_h = fh;
}

// The scaled copy has room for the whole screen, so resizing the window later cannot run out of memory.
static bool view_fit_enter(viewer_t *v, bool maximize)
{
    if (!sink_open(&v->fit, SCREEN_W, SCREEN_H)) return false;
    v->fit_on = true;
    v->fit_w = v->fit_h = 0;
    v->drag = false;
    v->fit_maximized = maximize && !(v->win->flags & WIN_MAXIMIZED);
    if (v->fit_maximized) wm_maximize(v->win, true);
    ui_invalidate(v->canvas);
    return true;
}

static void view_fit_leave(viewer_t *v)
{
    v->fit_on = false;
    free(v->fit.out);
    v->fit.out = NULL;
    sink_free_dither(&v->fit);
}

// Fit fills the screen, so it maximises the window; turning it off puts back what it changed.
static void view_set_fit(viewer_t *v, bool on)
{
    if (on == v->fit_on) return;
    if (on) {
        if (!view_fit_enter(v, true))
            msgbox(v->win, "Picture Viewer", "Not enough memory to fit the picture to the screen.", MB_ERROR, MB_OK);
        return;
    }
    view_fit_leave(v);
    if (v->fit_maximized && (v->win->flags & WIN_MAXIMIZED)) wm_maximize(v->win, false);
    v->fit_maximized = false;
    view_clamp(v);
    ui_invalidate(v->canvas);
}

// Where the picture's top left corner is on the canvas: centred if it is smaller, else scrolled.
static void view_origin(const viewer_t *v, int *ox, int *oy)
{
    int iw = v->w * v->zoom >> 8, ih = v->h * v->zoom >> 8;
    *ox = iw < v->canvas->r.w ? (v->canvas->r.w - iw) / 2 : -(v->sx * v->zoom >> 8);
    *oy = ih < v->canvas->r.h ? (v->canvas->r.h - ih) / 2 : -(v->sy * v->zoom >> 8);
}

static void view_paint(widget_t *w, gfx_t *g)
{
    // only the GUI task paints; too large for its stack next to everything else on it
    static pixel_t line[SCREEN_W];
    static uint16_t xmap[SCREEN_W];
    viewer_t *v = w->win->user;
    if (v->fit_on) {
        int fw, fh;
        view_fit_size(v, &fw, &fh);
        if (fw != v->fit_w || fh != v->fit_h) view_fit_render(v, fw, fh);
        if (fw < w->r.w || fh < w->r.h) gfx_fill(g, 0, 0, w->r.w, w->r.h, C_DKGREY);
        gfx_blit(g, (w->r.w - fw) / 2, (w->r.h - fh) / 2, fw, fh, v->fit.out, SCREEN_W);
        return;
    }
    int z = v->zoom, ox, oy;
    int iw = v->w * z >> 8, ih = v->h * z >> 8;
    view_origin(v, &ox, &oy);
    if (iw < w->r.w || ih < w->r.h) gfx_fill(g, 0, 0, w->r.w, w->r.h, C_DKGREY);
    if (z == 256) { gfx_blit(g, ox, oy, v->w, v->h, v->px, v->w); return; }
    // scaled: only the part inside the clip, each screen pixel taking the picture pixel it falls on
    int x0 = g->cx0 - g->ox, x1 = g->cx1 - g->ox, y0 = g->cy0 - g->oy, y1 = g->cy1 - g->oy;
    if (x0 < ox) x0 = ox;
    if (y0 < oy) y0 = oy;
    if (x1 > ox + iw) x1 = ox + iw;
    if (y1 > oy + ih) y1 = oy + ih;
    int n = x1 - x0;
    if (n > SCREEN_W) n = SCREEN_W;
    if (n <= 0) return;
    for (int i = 0; i < n; i++) xmap[i] = (x0 + i - ox) * 256 / z;
    int shown = -1;
    for (int y = y0; y < y1; y++) {
        int sy = (y - oy) * 256 / z;
        if (sy != shown) {
            const pixel_t *s = v->px + sy * v->w;
            for (int i = 0; i < n; i++) line[i] = s[xmap[i]];
            shown = sy;
        }
        gfx_blit(g, x0, y, n, 1, line, n);
    }
}

static void view_zoom(viewer_t *v, int z)
{
    if (v->fit_on) { view_set_fit(v, false); return; }  // zooming means leaving fit, back at the last zoom
    if (z > VIEW_ZOOM_MAX) z = VIEW_ZOOM_MAX;
    if (z == v->zoom) return;
    // keep the centre in place
    int cw = v->canvas->r.w, ch = v->canvas->r.h;
    int cx = v->sx + cw * 128 / v->zoom, cy = v->sy + ch * 128 / v->zoom;
    v->zoom = z;
    v->drag = false;
    v->sx = cx - cw * 128 / z;
    v->sy = cy - ch * 128 / z;
    view_clamp(v);
    ui_invalidate(v->canvas);
}

// The picture point that was under the fingers stays under them, wherever they go, so one gesture
// both zooms and moves. The smallest zoom shows the whole picture; pinching down to it ends in fit,
// which is the same view with the picture properly averaged, in the window as it is.
static void view_pinch(viewer_t *v, int x, int y, int scale, int phase)
{
    int fit_z = view_fit_zoom(v), min_z = fit_z < 256 ? fit_z : 256;
    if (phase == PINCH_BEGIN) {
        if (v->fit_on) {
            view_fit_leave(v);
            v->fit_maximized = false;           // the window stays as it is from here on
            v->zoom = min_z;
            v->sx = v->sy = 0;
        }
        v->drag = false;
        int ox, oy;
        view_origin(v, &ox, &oy);
        v->pinch_zoom = v->zoom;
        v->pinch_x = (x - ox) * 256 / v->zoom;
        v->pinch_y = (y - oy) * 256 / v->zoom;
        return;
    }
    if (phase == PINCH_END) {
        if (v->zoom <= min_z && fit_z < 256 && view_fit_enter(v, false)) { v->zoom = 256; v->sx = v->sy = 0; }
        return;
    }
    int z = v->pinch_zoom * scale >> 8;
    if (z > 240 && z < 272) z = 256;            // actual size is sharp: let it catch
    if (z < min_z) z = min_z;
    if (z > VIEW_ZOOM_MAX) z = VIEW_ZOOM_MAX;
    v->zoom = z;
    v->sx = v->pinch_x - x * 256 / z;
    v->sy = v->pinch_y - y * 256 / z;
    view_clamp(v);
    ui_invalidate(v->canvas);
}

static bool view_key(widget_t *w, const event_t *e)
{
    viewer_t *v = w->win->user;
    if (!e->down) return false;
    if ((e->ch == 'f' || e->ch == 'F') && !(e->mods & (MOD_CTRL | MOD_ALT))) { view_set_fit(v, !v->fit_on); return true; }
    // the whole picture is showing: nothing to scroll
    if (v->fit_on && e->key != K_ESC && e->ch != '+' && e->ch != '=' && e->ch != '-') return false;
    int step = (e->mods & MOD_CTRL) ? 1 : 24;
    switch (e->key) {
    case K_LEFT: v->sx -= step; break;
    case K_RIGHT: v->sx += step; break;
    case K_UP: v->sy -= step; break;
    case K_DOWN: v->sy += step; break;
    case K_PGUP: v->sy -= w->r.h * 256 / v->zoom; break;
    case K_PGDN: v->sy += w->r.h * 256 / v->zoom; break;
    case K_HOME: v->sx = v->sy = 0; break;
    case K_END: v->sx = v->w; v->sy = v->h; break;
    case K_ESC: wm_close(w->win); return true;
    default:
        // the keys go to the next whole size, from wherever a pinch left the zoom
        if (e->ch == '+' || e->ch == '=') { view_zoom(v, (v->zoom / 256 + 1) * 256); return true; }
        if (e->ch == '-') { view_zoom(v, v->zoom > 256 ? (v->zoom - 1) / 256 * 256 : v->zoom); return true; }
        return false;
    }
    view_clamp(v);
    ui_invalidate(w);
    return true;
}

static void view_mouse(widget_t *w, int type, int x, int y, int btn, int clicks)
{
    viewer_t *v = w->win->user;
    if (type == EV_PINCH) { view_pinch(v, x, y, btn, clicks); return; }
    if (type == EV_MOUSE_DOWN && (btn == BTN_LEFT || btn == BTN_RIGHT)) {
        if (btn == BTN_LEFT && clicks == 2) { view_set_fit(v, !v->fit_on); return; }
        if (!v->fit_on) { v->drag = true; v->dx = x; v->dy = y; v->drag_sx = v->sx; v->drag_sy = v->sy; }
    } else if (type == EV_MOUSE_MOVE && v->drag) {
        int sx = v->drag_sx + (v->dx - x) * 256 / v->zoom, sy = v->drag_sy + (v->dy - y) * 256 / v->zoom;
        if (sx != v->sx || sy != v->sy) { v->sx = sx; v->sy = sy; view_clamp(v); ui_invalidate(w); }
    } else if (type == EV_MOUSE_UP) v->drag = false;
    else if (type == EV_WHEEL && !v->fit_on) { v->sy -= btn * 24; view_clamp(v); ui_invalidate(w); }
}
static const canvas_ops_t view_ops = {view_paint, view_key, view_mouse};

enum { VC_FIT = 1 };
static const menu_item_t vm_view[] = {{"&Fit to Screen\tF", VC_FIT, NULL, NULL, 0}, {NULL, 0, NULL, NULL, 0}};
static const menu_item_t vm_bar[] = {{"&View", 0, vm_view, NULL, 0}, {NULL, 0, NULL, NULL, 0}};

static void view_command(window_t *w, int cmd)
{
    viewer_t *v = w->user;
    if (cmd == VC_FIT) view_set_fit(v, !v->fit_on);
}

static int view_menu_state(window_t *w, int cmd)
{
    viewer_t *v = w->user;
    return cmd == VC_FIT && v->fit_on ? MI_CHECKED : 0;
}

static void view_resize(window_t *w)
{
    viewer_t *v = w->user;
    ui_set_rect(v->canvas, 0, 0, wm_client_w(w), wm_client_h(w));
    view_clamp(v);
}
static void view_destroy(window_t *w)
{
    viewer_t *v = w->user;
    free(v->fit.out);
    sink_free_dither(&v->fit);
    free(v->px);
    free(v);
}

void app_viewer(const char *arg)
{
    char path[300] = "";
    if (arg && arg[0]) strlcpy(path, arg, sizeof(path));
    else {
        strcpy(path, DISK_ROOT "/Pictures");
        if (!file_dialog(NULL, false, "Open Picture", path, sizeof(path))) return;
    }
    const char *err;
    int iw = 0, ih = 0;
    wm_busy(true);
    pixel_t *px = pic_load(path, &iw, &ih, &err);
    wm_busy(false);
    if (!px) { msgbox(NULL, "Picture Viewer", err, MB_ERROR, MB_OK); return; }
    viewer_t *v = calloc(1, sizeof(viewer_t));
    v->px = px; v->w = iw; v->h = ih; v->zoom = 256;
    // the window fits the picture; a picture larger than the desktop gets the whole desktop (the
    // window opens maximised, and restores to the desktop size less its frame)
    int cw = iw < 160 ? 160 : iw, ch = ih < 100 ? 100 : ih;
    char title[96];
    snprintf(title, sizeof(title), "%s (%d x %d) - Picture Viewer", fs_basename(path), iw, ih);
    window_t *w = wm_create(title, cw, ch, WIN_RESIZABLE);
    w->icon = &icon16_image;
    w->user = v;
    v->win = w;
    wm_set_menubar(w, vm_bar);
    w->on_command = view_command;
    w->on_menu_state = view_menu_state;
    int fw = w->r.w - cw, fh = w->r.h - ch;         // the frame around the picture
    bool too_big = w->r.w > SCREEN_W || w->r.h > DESK_H;
    if (too_big) {
        cw = SCREEN_W - fw;
        ch = DESK_H - fh;
        wm_resize(w, SCREEN_W, DESK_H);
        wm_move(w, 0, 0);
    }
    v->canvas = ui_canvas(w, 0, 0, cw, ch, &view_ops, true);
    v->canvas->flags |= WF_WANT_PINCH;
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

// eject [usb|sd]: with no name, whichever one is mounted; with both mounted it has to be named.
static void term_eject(term_t *t, const char *which)
{
    bool usb = which && !strcasecmp(which, "usb");
#if BOARD_HAS_SD
    bool sd = which && !strcasecmp(which, "sd");
    if (which && !usb && !sd) { term_puts(t, "eject: use eject usb or eject sd\n"); return; }
    if (!which && storage_usb_ok() && storage_sd_ok()) {
        term_puts(t, "eject: both are mounted, use eject usb or eject sd\n");
        return;
    }
    if (!which) { usb = storage_usb_ok(); sd = storage_sd_ok(); }
    if (!usb && !sd) { term_puts(t, "eject: no USB stick or SD card mounted\n"); return; }
    if (sd) {
        if (!storage_sd_ok()) term_puts(t, "eject: no SD card mounted\n");
        else term_puts(t, storage_eject(SD_ROOT) ? "The SD card can be removed.\n" : "eject: the SD card could not be ejected\n");
        return;
    }
#else
    if (which && !usb) { term_puts(t, "eject: use eject or eject usb\n"); return; }
#endif
    if (!storage_usb_ok()) term_puts(t, "eject: no USB stick mounted\n");
    else term_puts(t, storage_eject(USB_ROOT) ? "The USB stick can be removed.\n" : "eject: the USB stick could not be ejected\n");
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
#if BOARD_HAS_SD
                     "eject [usb|sd]  unmount the USB stick or the SD card\n");
#else
                     "eject       unmount the USB stick\n");
#endif
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
        term_eject(t, argv[1]);
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
    if (type != EV_WHEEL && type != EV_TOUCH_SCROLL) return;
    t->scroll += type == EV_WHEEL ? btn * 3 : ui_touch_steps(btn, 16);
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
    t->canvas->flags |= WF_WANT_CHARS | WF_WANT_ENTER | WF_WANT_ESC | WF_WANT_TAB | WF_CARET | WF_TOUCH_SCROLL;
    t->canvas->flags &= ~WF_WANT_KEYUP;
    w->on_resize = term_resize;
    w->on_destroy = term_destroy;
    w->min_w = 300; w->min_h = 120;
    term_puts(t, OS_NAME " terminal. Type help for the commands.\n");
    wm_show(w);
}

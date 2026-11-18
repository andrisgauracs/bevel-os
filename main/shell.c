// The shell: desktop icons, taskbar, main menu, Alt+Tab switcher, global keys, shut-down dialog.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "wm.h"
#include "comp.h"
#include "os.h"
#include "apps.h"
#include "audio.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

uint8_t shell_desktop_color = RGB222_8(0, 2, 2);   // a palette byte, as saved in the settings

enum { SF_NONE, SF_DESKTOP, SF_TASKBAR };
static int s_focus = SF_DESKTOP;

// ---- desktop icons
#define CELL_W 78
#define CELL_H 66
#define ICONS_PER_COL ((DESK_H - 8) / CELL_H)
static int s_desk_apps[16];     // indices into os_apps
static int s_desk_n;
static int s_desk_sel;

// ---- taskbar
#define START_X 2
#define START_W 70
#define TB_BTN_X 78
#define CLOCK_W 74
#define MAX_TASKS 16
static window_t *s_tasks[MAX_TASKS];
static int s_ntasks;
static int s_tb_sel;            // 0 = menu button, 1.. = window buttons (keyboard focus)
static bool s_start_open;
static uint32_t s_menu_closed_ms;

// ---- Alt+Tab
static bool s_switching;
static window_t *s_sw_list[MAX_TASKS + 4];
static int s_sw_n, s_sw_sel;

static bool s_gui_clean;

// ---- volume icon, left of the clock (only on a board with sound)
#if BOARD_HAS_AUDIO
#define VOL_W 22
#define TRAY_W (VOL_W + 2)
#define VOL_X (SCREEN_W - CLOCK_W - 3 - TRAY_W)
#define VOL_STEP 5              // the same steps as the slider in Settings
#define VP_W 48
#define VP_H 150
#define VP_X (SCREEN_W - VP_W - 2 < VOL_X + (VOL_W - VP_W) / 2 ? SCREEN_W - VP_W - 2 : VOL_X + (VOL_W - VP_W) / 2)
#define VP_Y (DESK_H - VP_H - 2)
#define VP_TRACK_Y (VP_Y + 28)
#define VP_TRACK_H (VP_H - 42)
static bool s_vol_open, s_vol_drag, s_vol_dirty;
static int s_vol_from;          // the volume when the current drag began
#else
#define TRAY_W 0
#endif

bool shell_has_focus(void) { return s_focus != SF_NONE && !wm_active(); }

static void inval_taskbar(void) { comp_invalidate(0, DESK_H, SCREEN_W, TASKBAR_H); }

static void desk_cell(int i, rect_t *r)
{
    *r = rect_make(6 + (i / ICONS_PER_COL) * CELL_W, 8 + (i % ICONS_PER_COL) * CELL_H, CELL_W, CELL_H);
}

static void inval_icon(int i)
{
    rect_t r;
    if (i < 0 || i >= s_desk_n) return;
    desk_cell(i, &r);
    comp_invalidate_rect(&r);
}

static void set_focus(int f)
{
    if (s_focus == f) return;
    s_focus = f;
    inval_taskbar();
    inval_icon(s_desk_sel);
}

void shell_focus_desktop(void)
{
    if (wm_active()) wm_activate(NULL);
    s_focus = SF_DESKTOP;
    inval_icon(s_desk_sel);
    inval_taskbar();
}

static void focus_taskbar(int idx)
{
    if (wm_active()) wm_activate(NULL);
    s_focus = SF_TASKBAR;
    s_tb_sel = idx;
    inval_icon(s_desk_sel);
    inval_taskbar();
}

// ---------------------------------------------------------------- window list

static bool in_taskbar(const window_t *w)
{
    return !(w->flags & (WIN_DIALOG | WIN_HIDDEN | WIN_DEAD));
}

void shell_windows_changed(void)
{
    // keep creation order: drop the ones that are gone, append new ones
    int n = 0;
    for (int i = 0; i < s_ntasks; i++) {
        if (wm_valid(s_tasks[i]) && in_taskbar(s_tasks[i])) s_tasks[n++] = s_tasks[i];
    }
    s_ntasks = n;
    window_t *bottom = wm_top();
    while (bottom && bottom->prev) bottom = bottom->prev;
    for (window_t *w = bottom; w; w = w->next) {
        if (!in_taskbar(w)) continue;
        bool have = false;
        for (int i = 0; i < s_ntasks; i++) if (s_tasks[i] == w) have = true;
        if (!have && s_ntasks < MAX_TASKS) s_tasks[s_ntasks++] = w;
    }
    if (s_tb_sel > s_ntasks) s_tb_sel = s_ntasks;
    if (wm_active()) s_focus = SF_NONE;
    else if (s_focus == SF_NONE) s_focus = SF_DESKTOP;
    if (s_start_open && !menu_is_open()) { s_start_open = false; s_menu_closed_ms = os_uptime_ms(); }
    inval_taskbar();
}

static void task_btn_rect(int i, rect_t *r)
{
    int avail = SCREEN_W - TB_BTN_X - CLOCK_W - 8 - TRAY_W;
    int bw = s_ntasks ? avail / s_ntasks : 0;
    if (bw > 150) bw = 150;
    *r = rect_make(TB_BTN_X + i * bw, DESK_H + 4, bw - 3, TASKBAR_H - 6);
}

// ---------------------------------------------------------------- painting

void shell_paint_desktop(gfx_t *g, const rect_t *area)
{
#if BOARD_HAS_WALLPAPER
    wallpaper_paint(g, gfx_from8(shell_desktop_color));
#else
    gfx_fill(g, 0, 0, SCREEN_W, DESK_H, gfx_from8(shell_desktop_color));
#endif
    for (int i = 0; i < s_desk_n; i++) {
        rect_t r;
        desk_cell(i, &r);
        if (!rect_overlap(&r, area)) continue;
        const app_def_t *a = &os_apps[s_desk_apps[i]];
        bool sel = i == s_desk_sel && s_focus == SF_DESKTOP && !wm_active();
        int ix = r.x + (CELL_W - 32) / 2;
        if (sel) gfx_bitmap_tint(g, ix, r.y + 2, a->icon32, C_SEL); else gfx_bitmap(g, ix, r.y + 2, a->icon32);
        int tw = gfx_text_w(a->name, -1, FONT_UI);
        int tx = r.x + (CELL_W - tw) / 2, ty = r.y + 38;
        if (sel) {
            gfx_fill(g, tx - 3, ty - 2, tw + 5, FONT_UI_H + 3, C_SEL);
            gfx_dotted(g, tx - 3, ty - 2, tw + 5, FONT_UI_H + 3, C_YELLOW);
        } else if (i == s_desk_sel) {
            gfx_dotted(g, tx - 3, ty - 2, tw + 5, FONT_UI_H + 3, C_WHITE);
        } else {
            gfx_text(g, tx + 1, ty + 1, a->name, -1, FONT_UI, C_BLACK);
        }
        gfx_text(g, tx, ty, a->name, -1, FONT_UI, C_WHITE);
    }
}

// The time of day once the network - or, on a board with a clock chip, the chip - has supplied it;
// the uptime since power-on until then.
static void fmt_clock(char *buf, int n)
{
    if (clock_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(buf, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        return;
    }
    uint32_t s = os_uptime_ms() / 1000;
    snprintf(buf, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

void shell_paint_taskbar(gfx_t *g, const rect_t *area)
{
    gfx_fill(g, 0, DESK_H, SCREEN_W, TASKBAR_H, C_FACE);
    gfx_hline(g, 0, DESK_H, SCREEN_W, C_FACE);
    gfx_hline(g, 0, DESK_H + 1, SCREEN_W, C_HILIGHT);
    bool tb_focus = s_focus == SF_TASKBAR && !wm_active() && !menu_is_open();
    // menu button
    int by = DESK_H + 4, bh = TASKBAR_H - 6;
    bool pressed = s_start_open && menu_is_open();
    if (pressed) ui_draw_pressed(g, START_X, by, START_W, bh); else ui_draw_raised(g, START_X, by, START_W, bh);
    int o = pressed ? 1 : 0;
    gfx_bitmap(g, START_X + 5 + o, by + 3 + o, &icon16_logo);
    gfx_label(g, START_X + 25 + o, by + 5 + o, "&Bevel", FONT_UI_BOLD, C_TEXT, false);
    if (tb_focus && s_tb_sel == 0) gfx_dotted(g, START_X + 3, by + 3, START_W - 6, bh - 6, C_BLACK);
    // window buttons
    window_t *active = wm_active();
    for (int i = 0; i < s_ntasks; i++) {
        rect_t r;
        task_btn_rect(i, &r);
        window_t *w = s_tasks[i];
        bool act = w == active || (active && active->owner == w);
        if (act) { gfx_checker(g, r.x, r.y, r.w, r.h, C_HILIGHT, C_FACE); ui_draw_pressed(g, r.x, r.y, r.w, r.h); }
        else ui_draw_raised(g, r.x, r.y, r.w, r.h);
        gfx_state_t st;
        gfx_save(g, &st);
        if (gfx_clip(g, r.x + 3, r.y + 2, r.w - 6, r.h - 4)) {
            int tx = r.x + 5 + (act ? 1 : 0), ty = r.y + 5 + (act ? 1 : 0);
            if (w->icon) { gfx_bitmap(g, tx - 1, ty - 2, w->icon); tx += 19; }
            gfx_text(g, tx, ty, w->title, -1, act ? FONT_UI_BOLD : FONT_UI, C_TEXT);
        }
        gfx_restore(g, &st);
        if (tb_focus && s_tb_sel == i + 1) gfx_dotted(g, r.x + 3, r.y + 3, r.w - 6, r.h - 6, C_BLACK);
    }
#if BOARD_HAS_AUDIO
    if (s_vol_open) ui_draw_pressed(g, VOL_X, by, VOL_W, bh);
    gfx_bitmap(g, VOL_X + 3, by + 3, &icon16_sound);
    if (audio_volume == 0) gfx_line(g, VOL_X + 4, by + 17, VOL_X + 17, by + 4, C_RED);
#endif
    // clock
    int cx = SCREEN_W - CLOCK_W - 3;
    gfx_bevel(g, cx, by, CLOCK_W, bh, C_SHADOW, C_HILIGHT);
    char buf[16];
    fmt_clock(buf, sizeof(buf));
    gfx_text(g, cx + (CLOCK_W - gfx_text_w(buf, -1, FONT_UI)) / 2, by + 5, buf, -1, FONT_UI, C_TEXT);
}

#if BOARD_HAS_AUDIO
static void volume_paint(gfx_t *g)
{
    char t[8];
    gfx_fill(g, VP_X, VP_Y, VP_W, VP_H, C_FACE);
    ui_draw_raised(g, VP_X, VP_Y, VP_W, VP_H);
    snprintf(t, sizeof(t), "%d%%", audio_volume);
    gfx_text(g, VP_X + (VP_W - gfx_text_w(t, -1, FONT_UI)) / 2, VP_Y + 9, t, -1, FONT_UI, C_TEXT);
    int cx = VP_X + VP_W / 2;
    ui_draw_sunken(g, cx - 2, VP_TRACK_Y, 4, VP_TRACK_H);
    int ty = VP_TRACK_Y + VP_TRACK_H - audio_volume * VP_TRACK_H / 100;
    gfx_fill(g, cx - 10, ty - 5, 20, 10, C_FACE);
    ui_draw_raised(g, cx - 10, ty - 5, 20, 10);
}
#endif

void shell_paint_overlay(gfx_t *g, const rect_t *area)
{
#if BOARD_HAS_AUDIO
    if (s_vol_open) volume_paint(g);
#endif
    if (!s_switching) return;
    int w = 300, h = 34 + s_sw_n * 22;
    int x = (SCREEN_W - w) / 2, y = (DESK_H - h) / 2;
    gfx_fill(g, x, y, w, h, C_FACE);
    ui_draw_raised(g, x, y, w, h);
    gfx_text(g, x + 10, y + 8, "Switch to", -1, FONT_UI_BOLD, C_TEXT);
    for (int i = 0; i < s_sw_n; i++) {
        window_t *win = s_sw_list[i];
        int iy = y + 26 + i * 22;
        bool sel = i == s_sw_sel;
        if (sel) { gfx_fill(g, x + 6, iy, w - 12, 20, C_SEL); gfx_dotted(g, x + 6, iy, w - 12, 20, C_YELLOW); }
        if (win->icon) gfx_bitmap(g, x + 10, iy + 2, win->icon);
        gfx_state_t st;
        gfx_save(g, &st);
        if (gfx_clip(g, x + 32, iy, w - 44, 20)) gfx_text(g, x + 32, iy + 4, win->title, -1, FONT_UI, sel ? C_SEL_TEXT : C_TEXT);
        gfx_restore(g, &st);
    }
}

static void inval_switcher(void)
{
    int w = 300, h = 34 + s_sw_n * 22;
    comp_invalidate((SCREEN_W - w) / 2, (DESK_H - h) / 2, w, h);
}

// ---------------------------------------------------------------- main menu

#define CMD_APP_BASE 100
#define CMD_SHUTDOWN 1
#define CMD_SD_APP_BASE 300
static menu_item_t s_start_items[24];
static menu_item_t s_prog_items[40];

static void start_cb(void *ctx, int cmd)
{
    s_start_open = false;
    inval_taskbar();
    if (cmd == -1) { focus_taskbar(0); return; }   // Esc: keyboard stays on the menu button
    if (cmd == CMD_SHUTDOWN) shell_shutdown_dialog();
    else if (cmd >= CMD_SD_APP_BASE) app_launch_sd(cmd - CMD_SD_APP_BASE);
    else if (cmd >= CMD_APP_BASE) app_launch(cmd - CMD_APP_BASE, NULL);
}

void shell_open_start_menu(void)
{
    if (s_start_open && menu_is_open()) { menu_close_all(); return; }
    int n = 0, p = 0;
    for (int i = 0; i < os_app_count; i++) {
        if (os_apps[i].flags & APP_HIDDEN) continue;
        menu_item_t it = {os_apps[i].menu_label, CMD_APP_BASE + i, NULL, os_apps[i].icon16, 0};
        if (os_apps[i].flags & APP_IN_PROGRAMS) s_prog_items[p++] = it;
    }
    int nsd = app_sd_count();
    if (nsd && p < 38) s_prog_items[p++] = (menu_item_t){"-", 0, NULL, NULL, 0};
    for (int i = 0; i < nsd && p < 39; i++) {
        s_prog_items[p++] = (menu_item_t){app_sd_name(i), CMD_SD_APP_BASE + i, NULL, app_sd_is_game(i) ? &icon16_game : &icon16_app, 0};
    }
    s_prog_items[p] = (menu_item_t){NULL, 0, NULL, NULL, 0};
    s_start_items[n++] = (menu_item_t){"&Programs", 0, s_prog_items, &icon16_app, 0};
    s_start_items[n++] = (menu_item_t){"-", 0, NULL, NULL, 0};
    for (int i = 0; i < os_app_count; i++) {
        if (os_apps[i].flags & (APP_HIDDEN | APP_IN_PROGRAMS)) continue;
        s_start_items[n++] = (menu_item_t){os_apps[i].menu_label, CMD_APP_BASE + i, NULL, os_apps[i].icon16, 0};
    }
    s_start_items[n++] = (menu_item_t){"-", 0, NULL, NULL, 0};
    s_start_items[n++] = (menu_item_t){"Sh&ut Down...", CMD_SHUTDOWN, NULL, &icon16_shutdown, 0};
    s_start_items[n] = (menu_item_t){NULL, 0, NULL, NULL, 0};
    menu_popup(s_start_items, START_X, DESK_H + 3, true, start_cb, NULL);
    s_start_open = true;
    inval_taskbar();
}

// ---------------------------------------------------------------- shut down

static void halt_screen(void)
{
    pixel_t *fb = disp_fb();
    gfx_t g;
    disp_cursor_show(false);
    gfx_init(&g, fb, SCREEN_W, 0, 0, SCREEN_W, SCREEN_H);
    gfx_fill(&g, 0, 0, SCREEN_W, SCREEN_H, C_BLACK);
    const char *m1 = OS_NAME " has shut down.";
    const char *m2 = "It is safe to unplug it. Press any key to start again.";
    gfx_bitmap(&g, (SCREEN_W - 32) / 2, SCREEN_H / 2 - 60, &icon32_logo);
    gfx_text(&g, (SCREEN_W - gfx_text_w(m1, -1, FONT_UI_BOLD)) / 2, SCREEN_H / 2 - 14, m1, -1, FONT_UI_BOLD, C_ORANGE);
    gfx_text(&g, (SCREEN_W - gfx_text_w(m2, -1, FONT_UI)) / 2, SCREEN_H / 2 + 6, m2, -1, FONT_UI, C_GREY);
    disp_present(0, 0, SCREEN_W, SCREEN_H);
}

static void sd_btn(widget_t *w, int code) { wm_end_modal(w->win, w->id); }

void shell_shutdown_dialog(void)
{
    static bool open;
    if (open) return;
    open = true;
    window_t *d = wm_create("Shut Down " OS_NAME, 280, 126, WIN_DIALOG);
    d->icon = &icon16_shutdown;
    ui_icon(d, 14, 14, &icon32_shutdown);
    ui_label(d, 60, 12, 200, 14, "What do you want the computer to do?");
    widget_t *r1 = ui_radio(d, 60, 34, 200, "&Restart", 1, true);
    ui_radio(d, 60, 54, 200, "&Shut down", 1, false);
    widget_t *ok = ui_button(d, 104, 92, 78, 24, "OK", sd_btn);
    ok->id = ID_OK;
    widget_t *ca = ui_button(d, 190, 92, 78, 24, "Cancel", sd_btn);
    ca->id = ID_CANCEL;
    d->def_btn = ok; d->cancel_btn = ca;
    d->focus = r1;
    int choice = -1;
    if (wm_run_modal(d) == ID_OK) choice = ui_radio_selected(d, 1);
    open = false;
    if (choice < 0) return;
    os_shutdown_prepare();
    if (choice == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    halt_screen();
    serial_printf("@@HALT\n");
    event_t e;
    for (;;) {
        // nothing runs any more; any key press starts the computer again
        if (ev_wait(&e, 1000) && e.type == EV_KEY && e.down) esp_restart();
        if (e.type == EV_CALL && e.fn) { e.fn(e.arg); e.type = EV_NONE; }
    }
}

// ---------------------------------------------------------------- keyboard

static void switcher_start(bool back)
{
    s_sw_n = 0;
    for (window_t *w = wm_top(); w && s_sw_n < MAX_TASKS + 4; w = w->prev) {
        if (w->flags & (WIN_HIDDEN | WIN_DEAD)) continue;
        if (w->owner) continue; // reached through its owner
        s_sw_list[s_sw_n++] = w;
    }
    if (!s_sw_n) return;
    // minimised windows sink in the z-order but must still be reachable: they are in the list already
    bool on_window = wm_active() != NULL;
    s_sw_sel = on_window ? (back ? s_sw_n - 1 : (s_sw_n > 1 ? 1 : 0)) : 0;
    s_switching = true;
    if (menu_is_open()) menu_close_all();
    inval_switcher();
}

static void show_desktop(void)
{
    for (int i = 0; i < s_ntasks; i++) if (!(s_tasks[i]->flags & WIN_MINIMIZED)) wm_minimize(s_tasks[i]);
    shell_focus_desktop();
}

// ---------------------------------------------------------------- volume

#if BOARD_HAS_AUDIO
extern bool settings_save(void);
extern void settings_sound_changed(void);

static void volume_inval(void)
{
    comp_invalidate(VP_X, VP_Y, VP_W, VP_H);
    comp_invalidate(VOL_X, DESK_H + 4, VOL_W, TASKBAR_H - 6);
}

static void volume_set(int v)
{
    v = v < 0 ? 0 : v > 100 ? 100 : (v + VOL_STEP / 2) / VOL_STEP * VOL_STEP;
    if (v == audio_volume) return;
    audio_volume = v;
    s_vol_dirty = true;
    volume_inval();
    settings_sound_changed();
}

static void volume_from_y(int y)
{
    volume_set((VP_TRACK_Y + VP_TRACK_H - y) * 100 / VP_TRACK_H);
}

static void volume_close(void)
{
    s_vol_open = s_vol_drag = false;
    volume_inval();
    if (s_vol_dirty) settings_save();
    s_vol_dirty = false;
}

static void volume_step(int d)
{
    int v = audio_volume;
    volume_set(v + d);
    if (audio_volume != v) audio_chime(CHIME_INFO);
}

bool shell_volume_mouse(int type, int x, int y, int btn)
{
    if (!s_vol_open) return false;
    rect_t panel = rect_make(VP_X, VP_Y, VP_W, VP_H);
    rect_t icon = rect_make(VOL_X, DESK_H, VOL_W, TASKBAR_H);
    if (s_vol_drag) {
        if (type == EV_MOUSE_MOVE) volume_from_y(y);
        else if (type == EV_MOUSE_UP) {
            s_vol_drag = false;
            if (audio_volume != s_vol_from) audio_chime(CHIME_INFO);
        }
        return true;
    }
    bool inside = rect_has(&panel, x, y);
    if (type == EV_WHEEL) { volume_step(btn > 0 ? VOL_STEP : -VOL_STEP); return true; }
    if (type == EV_MOUSE_DOWN) {
        if (inside) {
            if (btn == BTN_LEFT) { s_vol_drag = true; s_vol_from = audio_volume; volume_from_y(y); }
            return true;
        }
        bool on_icon = rect_has(&icon, x, y);
        volume_close();
        return on_icon;     // the icon only closes it; any other click also does what it does
    }
    if (type == EV_MOUSE_MOVE && inside) wm_set_cursor(&cursor_arrow);
    return inside;
}

static bool volume_key(const event_t *e)
{
    switch (e->key) {
    case K_ESC: case K_ENTER: if (e->down) volume_close(); return true;
    case K_UP: case K_RIGHT: if (e->down) volume_step(VOL_STEP); return true;
    case K_DOWN: case K_LEFT: if (e->down) volume_step(-VOL_STEP); return true;
    case K_HOME: if (e->down) volume_step(100); return true;
    case K_END: if (e->down) volume_step(-100); return true;
    }
    return false;
}

static menu_item_t s_out_items[AUDIO_OUT_COUNT + 1];

static void output_cb(void *ctx, int cmd)
{
    if (cmd <= 0 || cmd - 1 == audio_output()) return;
    audio_set_output(cmd - 1);
    settings_save();
    settings_sound_changed();
    audio_chime(CHIME_INFO);    // played by the new output: the switch comes first in the mixer
}

static void output_menu(void)
{
    for (int i = 0; i < AUDIO_OUT_COUNT; i++)
        s_out_items[i] = (menu_item_t){audio_output_name(i), i + 1, NULL, NULL, i == audio_output() ? MI_RADIO : 0};
    s_out_items[AUDIO_OUT_COUNT] = (menu_item_t){NULL, 0, NULL, NULL, 0};
    menu_popup(s_out_items, VOL_X, DESK_H + 3, true, output_cb, NULL);
}
#else
bool shell_volume_mouse(int type, int x, int y, int btn) { return false; }
#endif

bool shell_key(const event_t *e)
{
    bool alt = e->mods & MOD_ALT, ctrl = e->mods & MOD_CTRL;
    bool is_gui = e->key == K_LGUI || e->key == K_RGUI;
#if BOARD_HAS_AUDIO
    if (s_vol_open && volume_key(e)) return true;
#endif
    if (s_switching) {
        if (!e->down && (e->key == K_LALT || e->key == K_RALT)) {
            inval_switcher();
            s_switching = false;
            window_t *w = s_sw_list[s_sw_sel];
            if (wm_valid(w)) wm_activate(w);
            return true;
        }
        if (!e->down) return true;
        inval_switcher();
        if (e->key == K_TAB || e->key == K_RIGHT || e->key == K_DOWN) {
            if (e->key == K_TAB && (e->mods & MOD_SHIFT)) s_sw_sel = (s_sw_sel + s_sw_n - 1) % s_sw_n;
            else s_sw_sel = (s_sw_sel + 1) % s_sw_n;
        } else if (e->key == K_LEFT || e->key == K_UP) s_sw_sel = (s_sw_sel + s_sw_n - 1) % s_sw_n;
        else if (e->key == K_ESC) s_switching = false;
        return true;
    }
    if (is_gui) {
        if (e->down) { if (!e->repeat) s_gui_clean = true; }
        else if (s_gui_clean) { s_gui_clean = false; shell_open_start_menu(); }
        return true;
    }
    if (!e->down) return false;
    s_gui_clean = false;
    if (e->mods & MOD_GUI) {
        if (e->key == K_D) show_desktop();
        else if (e->key == K_E) app_launch_name("Files", NULL);
        return true;
    }
    if (alt && !ctrl && e->key == K_TAB) { switcher_start(e->mods & MOD_SHIFT); return true; }
    if (ctrl && !alt && e->key == K_ESC) { shell_open_start_menu(); return true; }
    if (ctrl && alt && e->key == K_DEL) { if (menu_is_open()) menu_close_all(); app_launch_name("Tasks", NULL); return true; }
    if (alt && !ctrl && e->key == K_F4) {
        if (menu_is_open()) menu_close_all();
        if (wm_active()) wm_close(wm_active()); else shell_shutdown_dialog();
        return true;
    }
    if (alt && !ctrl && e->key == K_SPACE) {
        if (wm_active()) wm_window_menu(wm_active());
        return true;
    }
    if (e->key == K_F1 && !alt && !ctrl) { if (menu_is_open()) menu_close_all(); app_launch_name("Help", NULL); return true; }
    return false;
}

static void desk_open(int i)
{
    if (i >= 0 && i < s_desk_n) app_launch(s_desk_apps[i], NULL);
}

enum { DM_REFRESH = 1, DM_SETTINGS, DM_SYSTEM, DM_HELP };
static const menu_item_t s_desk_menu[] = {
    {"&Refresh", DM_REFRESH, NULL, NULL, 0},
    {"-", 0, NULL, NULL, 0},
    {"&Settings...", DM_SETTINGS, NULL, &icon16_settings, 0},
    {"S&ystem information", DM_SYSTEM, NULL, &icon16_computer, 0},
    {"&Help", DM_HELP, NULL, &icon16_help, 0},
    {NULL, 0, NULL, NULL, 0},
};
static void desk_menu_cb(void *ctx, int cmd)
{
    if (cmd == DM_REFRESH) { app_sd_rescan(); comp_invalidate_all(); }
    else if (cmd == DM_SETTINGS) app_launch_name("Settings", NULL);
    else if (cmd == DM_SYSTEM) app_launch_name("System", NULL);
    else if (cmd == DM_HELP) app_launch_name("Help", NULL);
}

bool shell_focus_key(const event_t *e)
{
    if (e->mods & (MOD_ALT | MOD_CTRL)) return false;
    if (s_focus == SF_TASKBAR) {
        int n = s_ntasks + 1;
        switch (e->key) {
        case K_LEFT: case K_UP: s_tb_sel = (s_tb_sel + n - 1) % n; inval_taskbar(); return true;
        case K_RIGHT: case K_DOWN: s_tb_sel = (s_tb_sel + 1) % n; inval_taskbar(); return true;
        case K_HOME: s_tb_sel = 0; inval_taskbar(); return true;
        case K_END: s_tb_sel = n - 1; inval_taskbar(); return true;
        case K_ENTER: case K_KP_ENTER: case K_SPACE:
            if (s_tb_sel == 0) shell_open_start_menu();
            else wm_activate(s_tasks[s_tb_sel - 1]);
            return true;
        case K_TAB: set_focus(SF_DESKTOP); return true;
        case K_ESC:
            for (window_t *w = wm_top(); w; w = w->prev) {
                if (!(w->flags & (WIN_HIDDEN | WIN_MINIMIZED | WIN_DEAD))) { wm_activate(w); return true; }
            }
            set_focus(SF_DESKTOP);
            return true;
        }
        return false;
    }
    // desktop icons
    int old = s_desk_sel, sel = s_desk_sel;
    switch (e->key) {
    case K_UP: if (sel % ICONS_PER_COL > 0) sel--; break;
    case K_DOWN: if (sel % ICONS_PER_COL < ICONS_PER_COL - 1 && sel + 1 < s_desk_n) sel++; break;
    case K_LEFT: if (sel >= ICONS_PER_COL) sel -= ICONS_PER_COL; break;
    case K_RIGHT: if (sel + ICONS_PER_COL < s_desk_n) sel += ICONS_PER_COL; else if (sel / ICONS_PER_COL < (s_desk_n - 1) / ICONS_PER_COL) sel = s_desk_n - 1; break;
    case K_HOME: sel = 0; break;
    case K_END: sel = s_desk_n - 1; break;
    case K_ENTER: case K_KP_ENTER: desk_open(sel); return true;
    case K_TAB: focus_taskbar(0); return true;
    case K_MENU: {
        rect_t r;
        desk_cell(sel, &r);
        menu_popup(s_desk_menu, r.x + 40, r.y + 30, false, desk_menu_cb, NULL);
        return true;
    }
    case K_F10:
        if (e->mods & MOD_SHIFT) { menu_popup(s_desk_menu, 100, 100, false, desk_menu_cb, NULL); return true; }
        return false;
    default:
        if (e->ch > ' ') {
            for (int k = 1; k <= s_desk_n; k++) {
                int i = (sel + k) % s_desk_n;
                if (tolower((unsigned char)os_apps[s_desk_apps[i]].name[0]) == tolower((unsigned char)e->ch)) { sel = i; break; }
            }
        } else return false;
    }
    if (sel != old) { s_desk_sel = sel; inval_icon(old); inval_icon(sel); }
    return true;
}

// ---------------------------------------------------------------- mouse

void shell_mouse(int type, int x, int y, int btn, int clicks)
{
    if (type != EV_MOUSE_DOWN) return;
    if (y >= DESK_H) {
        if (x >= START_X && x < START_X + START_W) {
            if (os_uptime_ms() - s_menu_closed_ms < 60) return; // this click just closed it
            shell_open_start_menu();
            return;
        }
#if BOARD_HAS_AUDIO
        if (x >= VOL_X && x < VOL_X + VOL_W) {
            if (btn == BTN_RIGHT) output_menu();
            else if (btn == BTN_LEFT) { s_vol_open = true; volume_inval(); }
            return;
        }
#endif
        for (int i = 0; i < s_ntasks; i++) {
            rect_t r;
            task_btn_rect(i, &r);
            if (!rect_has(&r, x, y)) continue;
            window_t *w = s_tasks[i];
            if (btn == BTN_RIGHT) { wm_activate(w); wm_window_menu(wm_active()); return; }
            window_t *a = wm_active();
            if ((a == w || (a && a->owner == w)) && !(w->flags & WIN_MINIMIZED)) { if (a == w) wm_minimize(w); }
            else wm_activate(w);
            return;
        }
        return;
    }
    // desktop
    int hit = -1;
    for (int i = 0; i < s_desk_n; i++) {
        rect_t r;
        desk_cell(i, &r);
        r.x += 8; r.w -= 16; r.h -= 10;
        if (rect_has(&r, x, y)) hit = i;
    }
    shell_focus_desktop();
    if (hit >= 0) {
        int old = s_desk_sel;
        s_desk_sel = hit;
        inval_icon(old); inval_icon(hit);
        if (btn == BTN_LEFT && clicks == 2) desk_open(hit);
    }
    if (btn == BTN_RIGHT) menu_popup(s_desk_menu, x, y, false, desk_menu_cb, NULL);
}

// ---------------------------------------------------------------- init

static void clock_tick(void *arg)
{
    comp_invalidate(SCREEN_W - CLOCK_W - 3, DESK_H + 4, CLOCK_W, TASKBAR_H - 6);
}

void shell_init(void)
{
    s_desk_n = 0;
    for (int i = 0; i < os_app_count && s_desk_n < 16; i++) {
        if (os_apps[i].flags & APP_ON_DESKTOP) s_desk_apps[s_desk_n++] = i;
    }
    timer_add(1000, true, clock_tick, NULL);
}
